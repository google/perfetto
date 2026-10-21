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

import {Time} from '../../base/time';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode, type Workspace} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, LONG_NULL, NUM, STR} from '../../trace_processor/query_result';
import TimeseriesTracksPlugin from '../dev.perfetto.TimeseriesTracks';
import {
  createTimeseriesRenderer,
  type TimeseriesCounter,
} from '../dev.perfetto.TimeseriesTracks/timeseries_renderer';
import {
  availableGroups,
  CHART_COUNTERS,
  type RowSpec,
  rowQuery,
  wantedTracks,
} from './tracks';

const WORKSPACE_NAME = 'Battery Historian';
const URI_PREFIX = 'com.android.BatteryHistorian#';

// Recreates Battery Historian's view of battery stats history: a chart of the
// battery level, temperature, voltage and charge above the device states and
// events (screen, wakelocks, radios, jobs, ...) as labelled rows, in a
// workspace of its own.
//
// Unlike com.android.AndroidLongBatteryTracing, which adds battery stats tracks
// to the default workspace next to everything else in a trace, this lays out
// only the battery stats history, the way Battery Historian does, for reading a
// bugreport's battery history at a glance.
export default class BatteryHistorianPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.BatteryHistorian';
  static readonly description =
    'Adds a Battery Historian workspace for traces with battery stats ' +
    'history, such as bugreports.';
  static readonly dependencies = [TimeseriesTracksPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    const tracks = await findTracks(trace);
    if (![...tracks.keys()].some((name) => name.startsWith('battery_stats.'))) {
      return;
    }
    await trace.engine.query(`INCLUDE PERFETTO MODULE android.battery_stats;`);

    const workspace = trace.workspaces.createEmptyWorkspace(WORKSPACE_NAME);
    addBatteryChart(trace, workspace, tracks);
    for (const group of availableGroups(new Set(tracks.keys()))) {
      const node = new TrackNode({
        name: group.name,
        isSummary: true,
        collapsed: false,
      });
      workspace.addChildLast(node);
      for (const row of group.rows) {
        node.addChildLast(await addRow(trace, group.name, row));
      }
    }

    const bounds = await historyBounds(trace, [...tracks.keys()]);
    trace.commands.registerCommand({
      id: 'com.android.BatteryHistorian#Open',
      name: 'Battery Historian: Open workspace',
      callback: () => {
        trace.workspaces.switchWorkspace(workspace);
        if (bounds !== undefined) {
          trace.timeline.panSpanIntoView(bounds.start, bounds.end, {
            align: 'zoom',
            margin: 0.02,
          });
        }
      },
    });
  }
}

// The tracks the workspace can show which the trace has, by name, with the id
// of the counter track (the first one, if there are several) for counters.
async function findTracks(trace: Trace): Promise<Map<string, number>> {
  const names = wantedTracks()
    .map((name) => `'${name}'`)
    .join(', ');
  const result = await trace.engine.query(`
    SELECT name, min(id) AS id
    FROM track
    WHERE name IN (${names})
    GROUP BY name
  `);
  const tracks = new Map<string, number>();
  for (const it = result.iter({name: STR, id: NUM}); it.valid(); it.next()) {
    tracks.set(it.name, it.id);
  }
  return tracks;
}

// The span of the battery stats history, which in a bugreport is usually much
// shorter than the logs around it.
async function historyBounds(trace: Trace, tracks: ReadonlyArray<string>) {
  const names = tracks.map((name) => `'${name}'`).join(', ');
  const result = await trace.engine.query(`
    WITH ts AS (
      SELECT c.ts FROM counter c JOIN track t ON c.track_id = t.id
      WHERE t.name IN (${names})
      UNION ALL
      SELECT s.ts + max(s.dur, 0) FROM slice s JOIN track t ON s.track_id = t.id
      WHERE t.name IN (${names})
    )
    SELECT min(ts) AS start, max(ts) AS end FROM ts
  `);
  const it = result.iter({start: LONG_NULL, end: LONG_NULL});
  if (it.start === null || it.end === null || it.start >= it.end) {
    return undefined;
  }
  return {start: Time.fromRaw(it.start), end: Time.fromRaw(it.end)};
}

// The battery level, temperature, voltage and charge, a row each.
function addBatteryChart(
  trace: Trace,
  workspace: Workspace,
  tracks: ReadonlyMap<string, number>,
) {
  const counters: TimeseriesCounter[] = [];
  for (const spec of CHART_COUNTERS) {
    const id = tracks.get(spec.track);
    if (id !== undefined) {
      counters.push({id, name: spec.name, unit: spec.unit, scale: spec.scale});
    }
  }
  if (counters.length === 0) return;

  const uri = `${URI_PREFIX}battery`;
  trace.tracks.registerTrack({
    uri,
    tags: {
      kinds: [COUNTER_TRACK_KIND],
      trackIds: counters.map((c) => c.id),
    },
    renderer: createTimeseriesRenderer(trace, uri, counters, {expanded: true}),
  });
  workspace.addChildLast(new TrackNode({uri, name: 'Battery'}));
}

async function addRow(
  trace: Trace,
  groupName: string,
  row: RowSpec,
): Promise<TrackNode> {
  const uri = `${URI_PREFIX}${groupName}/${row.name}`;
  const renderer = await SliceTrack.createMaterialized({
    trace,
    uri,
    dataset: new SourceDataset({
      src: rowQuery(row),
      schema: {ts: LONG, dur: LONG, name: STR},
    }),
  });
  trace.tracks.registerTrack({uri, renderer});
  return new TrackNode({uri, name: row.name});
}
