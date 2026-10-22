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

import {getOrCreate} from '../../base/utils';
import protos from '../../protos';
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {TrackNode} from '../../public/workspace';
import {
  createQueryResult,
  type QueryResult,
} from '../../trace_processor/query_result';
import FramesPlugin, {getProcessFrameTimelineUris} from './index';

const CELL_TYPE = protos.QueryResult.CellsBatch.CellType;

type Row = Readonly<Record<string, number | string>>;

// Builds a query result with the given columns and rows. Numbers are encoded as
// varints and strings as strings.
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

type ProcessRow = {
  readonly upid: number;
  readonly trackIds: string;
  readonly maxDepth: number;
};
const PROCESS_COLUMNS = ['upid', 'trackIds', 'maxDepth'];

type LayerRow = {
  readonly upid: number;
  readonly layerName: string;
  readonly maxDepth: number;
};
const LAYER_COLUMNS = ['upid', 'layerName', 'maxDepth'];

interface FakeTraceArgs {
  // Process level timelines, as returned by the per process summary queries.
  readonly expectedTimelines?: ReadonlyArray<ProcessRow>;
  readonly actualTimelines?: ReadonlyArray<ProcessRow>;
  // Per layer timelines, as returned by the layer discovery queries.
  readonly expectedLayers?: ReadonlyArray<LayerRow>;
  readonly actualLayers?: ReadonlyArray<LayerRow>;
}

// A trace whose engine answers each of the plugin's queries based on what it
// queries, and whose processes all have a (fake) process track group.
function fakeTrace(args: FakeTraceArgs) {
  const tracks = new Map<string, Track>();
  const groups = new Map<number, TrackNode>();
  const query = async (sql: string) => {
    if (sql.includes('internal_layout')) {
      return queryResult(
        LAYER_COLUMNS,
        sql.includes('expected_frame_timeline_slice')
          ? (args.expectedLayers ?? [])
          : (args.actualLayers ?? []),
      );
    }
    return queryResult(
      PROCESS_COLUMNS,
      sql.includes('android_expected_frame_timeline')
        ? (args.expectedTimelines ?? [])
        : (args.actualTimelines ?? []),
    );
  };
  const trace = {
    engine: {query},
    tracks: {
      registerTrack: (track: Track) => tracks.set(track.uri, track),
    },
    plugins: {
      getPlugin: () => ({
        getGroupForProcess: (upid: number) =>
          getOrCreate(
            groups,
            upid,
            () => new TrackNode({name: `process ${upid}`}),
          ),
      }),
    },
    selection: {registerAreaSelectionTab: () => {}},
  } as unknown as Trace;
  return {trace, tracks, groups};
}

async function loadTrace(args: FakeTraceArgs) {
  const fake = fakeTrace(args);
  await new FramesPlugin().onTraceLoad(fake.trace);
  return fake;
}

function findChild(node: TrackNode | undefined, name: string) {
  return node?.children.find((c) => c.name === name);
}

function childNames(node: TrackNode | undefined) {
  return node?.children.map((c) => c.name);
}

function setExperimentalJankEnabled(enabled: boolean) {
  FramesPlugin.showExperimentalJankClassification = {
    get: () => enabled,
  } as unknown as typeof FramesPlugin.showExperimentalJankClassification;
}

const PROCESS_1: ProcessRow = {upid: 1, trackIds: '10,11', maxDepth: 1};

beforeEach(() => setExperimentalJankEnabled(false));

describe('FramesPlugin layers', () => {
  test('groups the frame timelines of each layer under the actual timeline track', async () => {
    const {tracks, groups} = await loadTrace({
      expectedTimelines: [PROCESS_1],
      actualTimelines: [PROCESS_1],
      expectedLayers: [
        {upid: 1, layerName: 'StatusBar', maxDepth: 1},
        {upid: 1, layerName: 'NotificationShade', maxDepth: 2},
      ],
      actualLayers: [{upid: 1, layerName: 'StatusBar', maxDepth: 3}],
    });

    const actualTimeline = findChild(groups.get(1), 'Actual Timeline');

    // Layers are listed alphabetically as children of Actual Timeline.
    expect(childNames(actualTimeline)).toEqual([
      'NotificationShade',
      'StatusBar',
    ]);

    // A layer with both expected and actual frames gets both timelines.
    const statusBar = findChild(actualTimeline, 'StatusBar');
    expect(childNames(statusBar)).toEqual([
      'Expected Timeline',
      'Actual Timeline',
    ]);

    // A layer with expected frames only gets the expected timeline.
    const shade = findChild(actualTimeline, 'NotificationShade');
    expect(childNames(shade)).toEqual(['Expected Timeline']);

    // Tracks are registered under the uri referenced by the track nodes.
    const actualUri = findChild(statusBar, 'Actual Timeline')?.uri;
    expect(actualUri).toBe('/process_1/actual_frames/StatusBar');
    const actualTrack = tracks.get(actualUri!);
    expect(actualTrack).toBeDefined();

    // Layer tracks must not claim the process' track ids, otherwise they
    // shadow the process level timelines in the global track id index.
    expect(actualTrack?.tags?.trackIds).toBeUndefined();
    expect(tracks.get('/process_1/actual_frames')?.tags?.trackIds).toEqual([
      10, 11,
    ]);
  });

  test('escapes layer names in track uris', async () => {
    const {groups} = await loadTrace({
      actualTimelines: [PROCESS_1],
      actualLayers: [{upid: 1, layerName: 'TX - VRI#0', maxDepth: 1}],
    });

    const actualTimeline = findChild(groups.get(1), 'Actual Timeline');
    const layer = findChild(actualTimeline, 'TX - VRI#0');
    expect(findChild(layer, 'Actual Timeline')?.uri).toBe(
      '/process_1/actual_frames/TX%20-%20VRI%230',
    );
  });

  test('adds the experimental timeline only when the setting is on', async () => {
    const args: FakeTraceArgs = {
      actualTimelines: [PROCESS_1],
      actualLayers: [{upid: 1, layerName: 'StatusBar', maxDepth: 1}],
    };
    const layerChildren = async () => {
      const {groups} = await loadTrace(args);
      const actualTimeline = findChild(groups.get(1), 'Actual Timeline');
      return childNames(findChild(actualTimeline, 'StatusBar'));
    };

    expect(await layerChildren()).toEqual(['Actual Timeline']);

    setExperimentalJankEnabled(true);
    expect(await layerChildren()).toEqual([
      'Actual Timeline',
      'Actual Timeline (Experimental)',
    ]);
  });

  test('skips processes without an actual timeline track', async () => {
    const {tracks} = await loadTrace({
      expectedTimelines: [PROCESS_1],
      expectedLayers: [{upid: 1, layerName: 'StatusBar', maxDepth: 1}],
    });

    // Only the process level expected timeline is registered.
    expect(Array.from(tracks.keys())).toEqual(['/process_1/expected_frames']);
  });

  test('layer tracks are not reported as process level timelines', async () => {
    setExperimentalJankEnabled(true);
    const {tracks} = await loadTrace({
      actualTimelines: [PROCESS_1],
      expectedLayers: [{upid: 1, layerName: 'StatusBar', maxDepth: 1}],
      actualLayers: [{upid: 1, layerName: 'StatusBar', maxDepth: 1}],
    });

    // Other plugins rely on these uris to pick the process level timelines
    // without also picking the identically named layer tracks.
    const processUris = getProcessFrameTimelineUris(1);
    const layerUris = Array.from(tracks.keys()).filter(
      (uri) => !processUris.includes(uri),
    );
    expect(layerUris).toEqual([
      '/process_1/expected_frames/StatusBar',
      '/process_1/actual_frames/StatusBar',
      '/process_1/actual_frames_experimental/StatusBar',
    ]);
  });
});
