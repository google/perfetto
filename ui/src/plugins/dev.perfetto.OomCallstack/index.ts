// Copyright (C) 2024 The Android Open Source Project
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

import type { Trace } from '../../public/trace';
import type { PerfettoPlugin } from '../../public/plugin';
import { NUM, STR_NULL, STR } from '../../trace_processor/query_result';
import { TrackNode } from '../../public/workspace';
import {
  createPerfettoTable,
} from '../../trace_processor/sql_utils';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import type { Track } from '../../public/track';
import { FLAMEGRAPH_STATE_SCHEMA } from '../../widgets/flamegraph';
import type { Store } from '../../base/store';
import { z } from 'zod';
import { assertExists } from '../../base/assert';
import { OomCallstackDetailsPanel } from './oom_callstack_details_panel';
import { SliceTrack } from '../../components/tracks/slice_track';
import { SourceDataset } from '../../trace_processor/dataset';
import { LONG } from '../../trace_processor/query_result';
import { Time } from '../../base/time';
import { materialColorScheme } from '../../components/colorizer';

const EVENT_TABLE_NAME = 'oom_callstack_events';

const PLUGIN_STATE_SCHEMA = z.object({
  trackFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
});

type PluginState = z.infer<typeof PLUGIN_STATE_SCHEMA>;

// Helper function to generate a unique URI for the OOM callstack track of a specific process.
function trackUri(upid: number): string {
  return `/process_${upid}/oom_callstack`;
}

export default class OomCallstackPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.OomCallstack';

  // Used to ensure process groups folders exist before we add our track to them.
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  private store?: Store<PluginState>;

  async onTraceLoad(trace: Trace): Promise<void> {
    this.store = trace.mountStore(OomCallstackPlugin.id, (init) => {
      const result = PLUGIN_STATE_SCHEMA.safeParse(init);
      return result.data ?? {};
    });

    // Create the SQL table containing our OOM callstack events.
    await this.createEventTable(trace);

    // Add visual tracks to the UI timeline based on the data in our table.
    await this.addProcessTracks(trace);


  }

  private async createEventTable(trace: Trace) {
    await trace.engine.query('INCLUDE PERFETTO MODULE android.memory.heap_graph.oome;');

    await createPerfettoTable({
      engine: trace.engine,
      name: EVENT_TABLE_NAME,
      as: `
        SELECT
          g.id,
          -- Ensure the timestamp is at least the start of the trace.
          -- Subtract 50ms to make the icon clickable, as the OOM event often sits at the end of the timeline.
          MIN(g.ts, (SELECT end_ts - 50000000 FROM trace_bounds)) AS ts,
          g.ts AS reference_ts,
          -- Sometimes heap_graph points to a upid that was recycled, so we try to find the most recent process with the same pid.
          IFNULL((
            SELECT p.upid 
            FROM process p 
            JOIN process p2 ON p.pid = p2.pid 
            WHERE p2.upid = g.upid
              AND p.start_ts IS NOT NULL 
            ORDER BY p.end_ts DESC 
            LIMIT 1
          ), g.upid) AS upid,
          0 AS dur,   -- Our event is instantaneous, so duration is 0.
          0 AS depth, -- We only have one track layer, so depth is 0.
          'oom_callstack' AS type,
          (SELECT t.tid FROM heap_graph_thread_callsite hc JOIN thread t ON t.utid = hc.utid WHERE hc.heap_graph_id = g.id LIMIT 1) AS tid,
          (SELECT t.name FROM heap_graph_thread_callsite hc JOIN thread t ON t.utid = hc.utid WHERE hc.heap_graph_id = g.id LIMIT 1) AS thread_name
        FROM heap_graph g
        WHERE g.dump_reason = 'OOME'
      `,
    });
  }

  // Adds a track for every process that had an OOM.
  private async addProcessTracks(trace: Trace) {
    // Plugin responsible for grouping tracks by process.
    const trackGroupsPlugin = trace.plugins.getPlugin(ProcessThreadGroupsPlugin);

    // Find all unique process IDs (upids) that have an OOM event.
    const result = await trace.engine.query(`
      SELECT upid
      FROM ${EVENT_TABLE_NAME}
      GROUP BY upid
    `);

    for (const it = result.iter({ upid: NUM }); it.valid(); it.next()) {
      const upid = it.upid;

      let group = trackGroupsPlugin.getGroupForProcess(upid);
      if (!group) {
        // If the process has no other tracks, its group might not exist yet. Create it.
        const processResult = await trace.engine.query(`SELECT pid, name FROM process WHERE upid = ${upid}`);
        const row = processResult.firstRow({ pid: NUM, name: STR_NULL });
        const pid = row.pid;
        const name = row.name ?? `Uid ${upid}`;

        group = new TrackNode({
          uri: `process_${upid}`,
          name: `${name} ${pid}`,
          isSummary: true,
        });
        // Add the newly created process group to the main workspace.
        trace.defaultWorkspace.addChildInOrder(group);
      }

      const store = assertExists(this.store);
      const uri = trackUri(upid);

      // Define the track itself.
      const track: Track = {
        uri,
        tags: {
          upid: upid,
          kinds: ['oom_callstack'],
        },
        // SliceTrack creates a standard timeline track with slices (or instantaneous events).
        renderer: SliceTrack.create({
          trace,
          uri,
          dataset: new SourceDataset({
            src: EVENT_TABLE_NAME,
            schema: {
              ts: LONG,
              dur: LONG,
              type: STR,
              id: NUM,
              reference_ts: LONG,
            },
            filter: { col: 'upid', eq: upid },
          }),
          // When the user clicks on the event in the track.
          detailsPanel: (row) => {
            const ts = Time.fromRaw(row.reference_ts);
            return new OomCallstackDetailsPanel(
              trace,
              upid,
              ts,
              store.state.trackFlamegraphState,
              (state) => {
                store.edit((draft) => {
                  draft.trackFlamegraphState = state;
                });
              },
            );
          },
          tooltip: () => 'OOM callstack',
          // Colorize the slice based on its timestamp.
          colorizer: (slice) => materialColorScheme(slice.ts.toString()),
        }),
      };

      trace.tracks.registerTrack(track);

      const trackNode = new TrackNode({
        uri,
        name: 'OOM callstack',
        sortOrder: -30,
      });

      group.addChildInOrder(trackNode);
    }
  }


}
