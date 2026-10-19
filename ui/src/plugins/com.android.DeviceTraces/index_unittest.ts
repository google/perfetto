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
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {TrackNode} from '../../public/workspace';
import {
  createQueryResult,
  type QueryResult,
} from '../../trace_processor/query_result';
import DeviceTracesPlugin from './index';

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

function createFakeTrace() {
  const tracks = new Map<string, Track>();
  const defaultWorkspace = new TrackNode({name: 'root'});

  const query = async (sql: string): Promise<QueryResult> => {
    if (
      sql.includes("WHERE category = 'device_trace'") &&
      sql.includes('COUNT(*)')
    ) {
      return queryResult(['cnt'], [{cnt: 10}]);
    }
    if (sql.includes('SELECT DISTINCT SUBSTR(a.key, 7) AS arg_name')) {
      return queryResult(
        ['arg_name'],
        [
          {arg_name: 'android_id'},
          {arg_name: 'session_name'},
          {arg_name: 'subscription_name'},
        ],
      );
    }
    if (sql.includes('CREATE OR REPLACE PERFETTO TABLE')) {
      return queryResult([], []);
    }
    if (
      sql.includes(
        "EXTRACT_ARG(t.source_arg_set_id, 'description') GLOB 'dt_power|*'",
      )
    ) {
      return queryResult(
        ['track_id', 'description'],
        [
          {
            track_id: 100,
            description: 'dt_power|fleet_total|ALL|ALL|Total Power|12000.00',
          },
          {
            track_id: 101,
            description: 'dt_power|fleet_sub|ALL|Display|Display|5000.00',
          },
          {
            track_id: 200,
            description: 'dt_power|user_total|user_1|ALL|Total Power|1800.00',
          },
          {
            track_id: 201,
            description: 'dt_power|user_sub|user_1|Display|Display|900.00',
          },
          {
            track_id: 202,
            description: 'dt_power|user_sub|user_1|Cellular|Cellular|600.00',
          },
          {
            track_id: 203,
            description:
              'dt_power|user_rail|user_1|Display|VSYS_PWR_DISPLAY|900.00',
          },
          {
            track_id: 204,
            description:
              'dt_power|user_rail|user_1|Cellular|VSYS_PWR_MODEM|600.00',
          },
        ],
      );
    }
    if (
      sql.includes(
        "EXTRACT_ARG(t.source_arg_set_id, 'description') GLOB 'dt_state|*'",
      )
    ) {
      return queryResult(
        ['track_id', 'description'],
        [
          {track_id: 301, description: 'dt_state|user_1|Top app|15'},
          {track_id: 302, description: 'dt_state|user_1|Screen state|10'},
          {track_id: 303, description: 'dt_state|user_1|Suspend / resume|8'},
        ],
      );
    }
    if (sql.includes('GROUP BY col_android_id')) {
      return queryResult(
        ['val', 'trace_count'],
        [{val: 'user_1', trace_count: 5}],
      );
    }
    if (sql.includes('GROUP BY col_session_name')) {
      return queryResult(
        ['val', 'trace_count'],
        [
          {val: 'perfetto_aot_merged', trace_count: 3},
          {val: 'session_with_lightweight_battery_tracing', trace_count: 2},
        ],
      );
    }
    if (sql.includes('GROUP BY col_subscription_name')) {
      return queryResult(
        ['val', 'trace_count'],
        [{val: 'PERFETTO_BATTERY_TRACE_LONG', trace_count: 2}],
      );
    }
    if (sql.includes('SELECT DISTINCT track_id')) {
      return queryResult(['track_id'], [{track_id: 1}, {track_id: 301}]);
    }
    return queryResult([], []);
  };

  const trace = {
    engine: {query},
    tracks: {
      registerTrack: (track: Track) => tracks.set(track.uri, track),
    },
    defaultWorkspace,
    selection: {
      registerAreaSelectionTab: () => {},
    },
    commands: {
      registerCommand: () => {},
    },
    raf: {
      scheduleFullRedraw: () => {},
    },
  } as unknown as Trace;

  return {trace, tracks, defaultWorkspace};
}

async function flushPromises(): Promise<void> {
  await new Promise((resolve) => setTimeout(resolve, 0));
  await new Promise((resolve) => setTimeout(resolve, 0));
}

describe('DeviceTracesPlugin', () => {
  test('populates 24h battery power rails and device state tracks under breakdown nodes', async () => {
    const {trace, tracks, defaultWorkspace} = createFakeTrace();
    const plugin = new DeviceTracesPlugin();
    await plugin.onTraceLoad(trace);

    expect(defaultWorkspace.children.map((c) => c.name)).toEqual([
      'Device Traces (session_name -> subscription_name -> android_id)',
      'Device Traces (android_id -> session_name -> subscription_name)',
    ]);

    // Expand Tree 2: android_id -> session_name -> subscription_name
    const tree2 = defaultWorkspace.children[1];
    tree2.expand();
    await flushPromises();

    expect(tree2.children.map((c) => c.name)).toEqual(['user_1 (5)']);
    const userNode = tree2.children[0];
    userNode.expand();
    await flushPromises();

    expect(userNode.children.map((c) => c.name)).toEqual([
      'perfetto_aot_merged (3)',
      'session_with_lightweight_battery_tracing (2)',
    ]);

    const batterySessionNode = userNode.children[1];
    batterySessionNode.expand();
    await flushPromises();

    // At session_name level, only subscription_name child appears (no duplicate Power Rails / Device State)
    expect(batterySessionNode.children.map((c) => c.name)).toEqual([
      'PERFETTO_BATTERY_TRACE_LONG (2)',
    ]);

    // Expand subscription_name leaf node to get Power Rails and Device State
    const subLeafNode = batterySessionNode.children[0];
    subLeafNode.expand();
    await flushPromises();

    expect(subLeafNode.children.map((c) => c.name)).toEqual([
      'Power Rails (mW) — Total 1.80 Wh (2 rails)',
      'Device State (3 tracks)',
    ]);

    // Expand Power Rails node to verify subsystem and individual rail tracks
    const powerRailsNode = subLeafNode.children[0];
    expect(powerRailsNode.headless).toBe(false);
    expect(powerRailsNode.uri).toBeDefined();
    expect(tracks.has(powerRailsNode.uri!)).toBe(true);

    powerRailsNode.expand();
    await flushPromises();

    expect(powerRailsNode.children.map((c) => c.name)).toEqual([
      'Subsystem: Display (900.0 mWh, 1 rails)',
      'Subsystem: Cellular (600.0 mWh, 1 rails)',
      'All Individual Rails (2)',
    ]);
    expect(powerRailsNode.children[2].headless).toBe(false);

    const displaySubNode = powerRailsNode.children[0];
    displaySubNode.expand();
    await flushPromises();
    expect(displaySubNode.children.map((c) => c.name)).toEqual([
      'VSYS_PWR_DISPLAY (900.0 mWh)',
    ]);

    // Expand Device State node and verify canonical ordering (Screen state, Suspend / resume, Top app)
    const deviceStateNode = subLeafNode.children[1];
    expect(deviceStateNode.headless).toBe(false);
    deviceStateNode.expand();
    await flushPromises();
    expect(deviceStateNode.children.map((c) => c.name)).toEqual([
      'Screen state (10)',
      'Suspend / resume (8)',
      'Top app (15)',
    ]);
    for (const stChild of deviceStateNode.children) {
      expect(stChild.headless).toBe(false);
      expect(stChild.uri).toBeDefined();
      expect(tracks.has(stChild.uri!)).toBe(true);
    }
  });
});
