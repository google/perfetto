// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {ensureExists, ensureIsInstance} from '../../base/assert';
import {okResult} from '../../base/result';
import {Time} from '../../base/time';
import {getOrCreate} from '../../base/utils';
import protos from '../../protos';
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {TrackNode, Workspace} from '../../public/workspace';
import {
  createQueryResult,
  type QueryResult,
} from '../../trace_processor/query_result';
import ProcessState from './index';
import {ProcessStateCountTrack} from './state_count_track';

const CELL_TYPE = protos.QueryResult.CellsBatch.CellType;

type Row = Readonly<Record<string, number | string>>;

function queryResult(
  columnNames: ReadonlyArray<string>,
  rows: ReadonlyArray<Row>,
): QueryResult {
  const cells = rows.flatMap((row) =>
    columnNames.map((col) =>
      typeof row[col] === 'number'
        ? CELL_TYPE.CELL_VARINT
        : CELL_TYPE.CELL_STRING,
    ),
  );
  const values = rows.flatMap((row) => columnNames.map((col) => row[col]));
  const batch = protos.QueryResult.CellsBatch.create({
    cells,
    varintCells: values.filter((v) => typeof v === 'number'),
    stringCells: values.filter((v) => typeof v === 'string').join('\0'),
    isLastBatch: true,
  });
  const result = createQueryResult({query: 'fake'});
  result.appendResultBatch(
    protos.QueryResult.encode(
      protos.QueryResult.create({
        columnNames: [...columnNames],
        batch: [batch],
      }),
    ).finish(),
  );
  return result;
}

interface BreakdownRow extends Row {
  readonly upid: number;
  readonly pid: number;
  readonly name: string;
}

function fakeTrace(onBreakdownQuery: (sql: string) => BreakdownRow[]) {
  const tracks = new Map<string, Track>();
  const groups = new Map<number, TrackNode>();
  const defaultWorkspace = new Workspace('test');

  const query = async (sql: string) => {
    if (sql.includes('INCLUDE PERFETTO MODULE')) {
      return queryResult(['ok'], [{ok: 1}]);
    }
    if (sql.includes('hasFreezer')) {
      return queryResult(
        ['upid', 'pid', 'name', 'hasFreezer'],
        [
          {upid: 1, pid: 101, name: 'com.example.alpha', hasFreezer: 1},
          {upid: 2, pid: 202, name: 'com.example.beta', hasFreezer: 0},
        ],
      );
    }
    if (sql.includes('FROM _android_process_capability_intervals')) {
      return queryResult(
        ['upid', 'capability'],
        [{upid: 1, capability: 'CAMERA'}],
      );
    }
    if (sql.includes('FROM _android_process_state_concurrency')) {
      return queryResult(['state', 'rank'], [{state: 'TOP', rank: 2}]);
    }
    if (sql.includes('FROM _android_freezer_concurrency')) {
      return queryResult(['state'], [{state: 'FROZEN'}]);
    }
    if (sql.includes('LEFT JOIN android_process_metadata')) {
      return queryResult(['upid', 'pid', 'name'], onBreakdownQuery(sql));
    }
    return queryResult([], []);
  };

  const trace = {
    engine: {
      tryQuery: async () => okResult(queryResult(['cnt'], [{cnt: 1}])),
      query,
    },
    tracks: {
      registerTrack: (track: Track) => {
        if (tracks.has(track.uri)) {
          throw new Error(`Duplicate track URI: ${track.uri}`);
        }
        tracks.set(track.uri, track);
      },
      getTrack: (uri: string) => tracks.get(uri),
    },
    plugins: {
      getPlugin: () => ({
        getGroupForProcess: (upid: number) =>
          upid === 1
            ? getOrCreate(
                groups,
                upid,
                () => new TrackNode({name: `process ${upid}`}),
              )
            : undefined,
      }),
    },
    selection: {
      selection: {kind: 'empty'},
      registerAreaSelectionTab: () => {},
      registerSqlSelectionResolver: () => {},
    },
    defaultWorkspace,
  } as unknown as Trace;

  return {trace, tracks, defaultWorkspace};
}

describe('ProcessState breakdown tracks', () => {
  test('attaches per-process timelines under framework and freezer counters and cleans up on replace/clear', async () => {
    let activeRows: BreakdownRow[] = [
      {upid: 1, pid: 101, name: 'com.example.alpha'},
      {upid: 2, pid: 202, name: 'com.example.beta'},
    ];
    const {trace, tracks, defaultWorkspace} = fakeTrace(() => activeRows);
    await new ProcessState().onTraceLoad(trace);

    const root = ensureExists(defaultWorkspace.children[0]);
    expect(root.children.map((c) => c.name)).toEqual([
      'By state',
      'By process',
    ]);
    const byState = ensureExists(root.children[0]);
    const topNode = ensureExists(
      byState.children.find((c) => c.name === 'TOP'),
    );
    const frozenNode = ensureExists(
      byState.children.find((c) => c.name === 'FROZEN'),
    );

    const topTrack = ensureIsInstance(
      tracks.get(ensureExists(topNode.uri))?.renderer,
      ProcessStateCountTrack,
    );
    await topTrack.showBreakdownForRange(Time.fromRaw(10n), Time.fromRaw(20n));

    expect(topNode.expanded).toBe(true);
    expect(topNode.children.map((c) => c.name)).toEqual([
      'com.example.alpha 101',
      'com.example.beta 202',
    ]);
    expect(topNode.children.every((c) => c.removable)).toBe(true);
    // Only alpha has freezer intervals, so only alpha gets a Freezer child.
    expect(
      ensureExists(topNode.children[0]).children.map((c) => c.name),
    ).toEqual(['Freezer']);
    expect(ensureExists(topNode.children[1]).children).toEqual([]);

    const alphaTopUri = 'com.android.ProcessState#count.TOP.process.1';
    const betaTopUri = 'com.android.ProcessState#count.TOP.process.2';
    expect(byState.getTrackByUri(alphaTopUri)?.id).toBe(
      topNode.children[0]?.id,
    );
    expect(tracks.get(alphaTopUri)?.renderer.rootTableName).toBeUndefined();
    expect(
      tracks.get('com.android.ProcessState#process.1')?.renderer.rootTableName,
    ).toBe('_android_process_state_intervals');

    // Replacing with a narrower window removes beta cleanly from ancestor maps
    // and reuses alpha's already-registered track without throwing.
    activeRows = [{upid: 1, pid: 101, name: 'com.example.alpha'}];
    await topTrack.showBreakdownForRange(Time.fromRaw(30n), Time.fromRaw(40n));
    expect(topNode.children.map((c) => c.name)).toEqual([
      'com.example.alpha 101',
    ]);
    expect(byState.getTrackByUri(betaTopUri)).toBeUndefined();
    expect(defaultWorkspace.getTrackByUri(betaTopUri)).toBeUndefined();

    topTrack.clearBreakdown();
    expect(topNode.hasChildren).toBe(false);
    expect(topNode.collapsed).toBe(true);
    expect(byState.getTrackByUri(alphaTopUri)).toBeUndefined();

    // FROZEN counter attaches freezer timelines directly as removable children.
    const frozenTrack = ensureIsInstance(
      tracks.get(ensureExists(frozenNode.uri))?.renderer,
      ProcessStateCountTrack,
    );
    await frozenTrack.showBreakdownAtInstant(Time.fromRaw(15n));
    expect(frozenNode.expanded).toBe(true);
    expect(frozenNode.children.map((c) => c.name)).toEqual([
      'com.example.alpha 101',
    ]);
    const frozenAlpha = ensureExists(frozenNode.children[0]);
    expect(frozenAlpha.children).toEqual([]);
    expect(frozenAlpha.uri).toBe(
      'com.android.ProcessState#count.FROZEN.process.1',
    );
  });
});
