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

import protos from '../../protos';
import type {App} from '../../public/app';
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {TrackNode} from '../../public/workspace';
import {
  createQueryResult,
  type QueryResult,
} from '../../trace_processor/query_result';
import FramesPlugin from './index';

const CELL_TYPE = protos.QueryResult.CellsBatch.CellType;

interface LayerRow {
  readonly upid: number;
  readonly layerName: string;
  readonly trackIds: string;
  readonly maxDepth: number;
}

// Builds a query result with the columns returned by the 'frames by layer'
// queries: (upid, layerName, trackIds, maxDepth).
function layerQueryResult(rows: ReadonlyArray<LayerRow>): QueryResult {
  const batch = protos.QueryResult.CellsBatch.create({
    cells: rows.flatMap(() => [
      CELL_TYPE.CELL_VARINT,
      CELL_TYPE.CELL_STRING,
      CELL_TYPE.CELL_STRING,
      CELL_TYPE.CELL_VARINT,
    ]),
    varintCells: rows.flatMap((r) => [r.upid, r.maxDepth]),
    stringCells: rows.flatMap((r) => [r.layerName, r.trackIds]).join('\0'),
    isLastBatch: true,
  });
  const result = createQueryResult({query: 'frames by layer'});
  result.appendResultBatch(
    protos.QueryResult.encode(
      protos.QueryResult.create({
        columnNames: ['upid', 'layerName', 'trackIds', 'maxDepth'],
        batch: [batch],
      }),
    ).finish(),
  );
  return result;
}

// A trace whose engine replies to the two 'frames by layer' queries, in the
// order the plugin issues them: expected frames first, actual frames second.
function fakeTrace(args: {
  readonly expected: ReadonlyArray<LayerRow>;
  readonly actual: ReadonlyArray<LayerRow>;
  readonly processGroup?: TrackNode;
}) {
  const results = [
    layerQueryResult(args.expected),
    layerQueryResult(args.actual),
  ];
  const tracks = new Map<string, Track>();
  const trace = {
    engine: {query: async () => results.shift()},
    tracks: {
      registerTrack: (track: Track) => tracks.set(track.uri, track),
    },
    plugins: {
      getPlugin: () => ({getGroupForProcess: () => args.processGroup}),
    },
  } as unknown as Trace;
  return {trace, tracks};
}

function findChild(node: TrackNode | undefined, name: string) {
  return node?.children.find((c) => c.name === name);
}

function setExperimentalJankEnabled(enabled: boolean) {
  FramesPlugin.showExperimentalJankClassification = {
    get: () => enabled,
  } as unknown as typeof FramesPlugin.showExperimentalJankClassification;
}

beforeEach(() => setExperimentalJankEnabled(false));

describe('FramesPlugin settings', () => {
  test('registers the group frames by layer setting, off by default', () => {
    const settings: Array<{id: string; defaultValue: unknown}> = [];
    const app = {
      settings: {
        register: (config: {id: string; defaultValue: unknown}) => {
          settings.push(config);
          return {get: () => config.defaultValue};
        },
      },
    } as unknown as App;

    FramesPlugin.onActivate(app);

    const setting = settings.find(
      (s) => s.id === 'dev.perfetto.Frames#groupFramesByLayer',
    );
    expect(setting).toBeDefined();
    expect(setting?.defaultValue).toBe(false);
  });
});

describe('FramesPlugin.addFramesByLayer', () => {
  test('groups the frame timelines of each layer under the actual timeline track', async () => {
    const processGroup = new TrackNode({name: 'Process 1'});
    const actualTimeline = new TrackNode({
      uri: '/process_1/actual_frames',
      name: 'Actual Timeline',
      sortOrder: -50,
    });
    processGroup.addChildInOrder(actualTimeline);

    const {trace, tracks} = fakeTrace({
      expected: [
        {upid: 1, layerName: 'StatusBar', trackIds: '10', maxDepth: 1},
        {upid: 1, layerName: 'NotificationShade', trackIds: '11', maxDepth: 2},
      ],
      actual: [
        {upid: 1, layerName: 'StatusBar', trackIds: '20,21', maxDepth: 3},
      ],
      processGroup,
    });

    await new FramesPlugin().addFramesByLayer(trace);

    // Layers are listed alphabetically as children of Actual Timeline.
    expect(actualTimeline.children.map((c) => c.name)).toEqual([
      'NotificationShade',
      'StatusBar',
    ]);

    // A layer with both expected and actual frames gets both timelines.
    const statusBar = findChild(actualTimeline, 'StatusBar');
    expect(statusBar?.children.map((c) => c.name)).toEqual([
      'Expected Timeline',
      'Actual Timeline',
    ]);

    // A layer with expected frames only gets the expected timeline.
    const shade = findChild(actualTimeline, 'NotificationShade');
    expect(shade?.children.map((c) => c.name)).toEqual(['Expected Timeline']);

    // Tracks are registered under the uri referenced by the track nodes, and
    // are scoped to the track ids of that layer.
    const actualUri = findChild(statusBar, 'Actual Timeline')?.uri;
    expect(actualUri).toBe('/process_1/actual_frames/StatusBar');
    expect(tracks.get(actualUri!)?.tags?.trackIds).toEqual([20, 21]);
  });

  test('escapes layer names in track uris', async () => {
    const processGroup = new TrackNode({name: 'Process 1'});
    const actualTimeline = new TrackNode({
      uri: '/process_1/actual_frames',
      name: 'Actual Timeline',
    });
    processGroup.addChildInOrder(actualTimeline);

    const {trace} = fakeTrace({
      expected: [],
      actual: [{upid: 1, layerName: 'TX - VRI#0', trackIds: '20', maxDepth: 1}],
      processGroup,
    });

    await new FramesPlugin().addFramesByLayer(trace);

    const layer = findChild(actualTimeline, 'TX - VRI#0');
    expect(findChild(layer, 'Actual Timeline')?.uri).toBe(
      '/process_1/actual_frames/TX%20-%20VRI%230',
    );
  });

  test('adds the experimental timeline only when the setting is on', async () => {
    const layers = {
      expected: [],
      actual: [{upid: 1, layerName: 'StatusBar', trackIds: '20', maxDepth: 1}],
    };
    const makeProcessGroupWithActual = () => {
      const group = new TrackNode({name: 'Process 1'});
      group.addChildInOrder(
        new TrackNode({
          uri: '/process_1/actual_frames',
          name: 'Actual Timeline',
        }),
      );
      return group;
    };
    const layerNodeOf = (processGroup: TrackNode) =>
      findChild(findChild(processGroup, 'Actual Timeline'), 'StatusBar');

    const withSettingOff = makeProcessGroupWithActual();
    await new FramesPlugin().addFramesByLayer(
      fakeTrace({...layers, processGroup: withSettingOff}).trace,
    );
    expect(
      findChild(layerNodeOf(withSettingOff), 'Actual Timeline (Experimental)'),
    ).toBeUndefined();

    setExperimentalJankEnabled(true);
    const withSettingOn = makeProcessGroupWithActual();
    await new FramesPlugin().addFramesByLayer(
      fakeTrace({...layers, processGroup: withSettingOn}).trace,
    );
    // Sorts just below the non-experimental timelines.
    expect(
      findChild(layerNodeOf(withSettingOn), 'Actual Timeline (Experimental)')
        ?.sortOrder,
    ).toBe(-49);
  });

  test('skips processes without a process group', async () => {
    const {trace, tracks} = fakeTrace({
      expected: [],
      actual: [{upid: 1, layerName: 'StatusBar', trackIds: '20', maxDepth: 1}],
      processGroup: undefined,
    });

    await new FramesPlugin().addFramesByLayer(trace);

    expect(tracks.size).toBe(0);
  });
});
