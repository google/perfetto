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

// The name of the Perfetto SQL table we create to store our OOM callstack events.
const EVENT_TABLE_NAME = 'oom_callstack_events';

// Defines the shape of the plugin's state that is saved across reloads (e.g., when sharing a trace link).
const PLUGIN_STATE_SCHEMA = z.object({
  trackFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
});

type PluginState = z.infer<typeof PLUGIN_STATE_SCHEMA>;

// Helper function to generate a unique URI for the OOM callstack track of a specific process.
function trackUri(upid: number): string {
  return `/process_${upid}/oom_callstack`;
}

// The main plugin class. It implements PerfettoPlugin which enables the Perfetto UI
// to discover and initialize it.
export default class OomCallstackPlugin implements PerfettoPlugin {
  // A unique identifier for the plugin.
  static readonly id = 'dev.perfetto.OomCallstack';

  // This plugin depends on the ProcessThreadGroupsPlugin to ensure process groups 
  // (the folders grouping tracks per process) exist before we add our track.
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  // A store for persisting the plugin's state (like UI selection/flamegraph state).
  private store?: Store<PluginState>;

  // This method is called once when a trace is loaded into the UI.
  async onTraceLoad(trace: Trace): Promise<void> {
    // Mount the state store for this plugin so it's persisted in the UI state.
    this.store = trace.mountStore(OomCallstackPlugin.id, (init) => {
      const result = PLUGIN_STATE_SCHEMA.safeParse(init);
      return result.data ?? {};
    });

    // 1. Create the SQL table containing our OOM callstack events.
    await this.createEventTable(trace);

    // 2. Add visual tracks to the UI timeline based on the data in our table.
    await this.addProcessTracks(trace);


  }

  // Executes a SQL query to build the 'oom_callstack_events' table.
  private async createEventTable(trace: Trace) {
    // We need to include the module that defines 'android_heap_graph_java_oome_details' 
    // and other OOM related views.
    await trace.engine.query('INCLUDE PERFETTO MODULE android.memory.heap_graph.oome;');

    // createPerfettoTable runs the given SQL and creates a Perfetto table from the results.
    await createPerfettoTable({
      engine: trace.engine,
      name: EVENT_TABLE_NAME,
      as: `
        SELECT
          id,
          -- Ensure the timestamp is at least the start of the trace.
          MIN(ts, (SELECT end_ts FROM trace_bounds)) AS ts,
          ts AS reference_ts,
          -- Find the upid of the process that OOM'd. Sometimes heap_graph points to a upid 
          -- that was recycled, so we try to find the most recent process with the same pid.
          IFNULL((
            SELECT p.upid 
            FROM process p 
            JOIN process p2 ON p.pid = p2.pid 
            WHERE p2.upid = heap_graph.upid 
              AND p.start_ts IS NOT NULL 
            ORDER BY p.end_ts DESC 
            LIMIT 1
          ), upid) AS upid,
          0 AS dur,   -- Our event is instantaneous, so duration is 0.
          0 AS depth, -- We only have one track layer, so depth is 0.
          'oom_callstack' AS type
        FROM heap_graph
        -- We only care about heap graphs generated due to an OutOfMemoryError.
        WHERE dump_reason = 'OOME'
      `,
    });
  }

  // Reads the generated table and adds a track for every process that had an OOM.
  private async addProcessTracks(trace: Trace) {
    // Get the plugin responsible for grouping tracks by process.
    const trackGroupsPlugin = trace.plugins.getPlugin(ProcessThreadGroupsPlugin);

    // Find all unique process IDs (upids) that have an OOM event.
    const upidResult = await trace.engine.query(`
      SELECT DISTINCT upid
      FROM ${EVENT_TABLE_NAME}
    `);

    const upids: number[] = [];
    for (const it = upidResult.iter({ upid: NUM }); it.valid(); it.next()) {
      upids.push(it.upid);
    }

    // Loop through each process that had an OOM.
    for (const upid of upids) {

      // Attempt to find the existing workspace group for this process.
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
          kinds: ['oom_callstack'], // Tag used for internal categorization.
        },
        // SliceTrack creates a standard timeline track with slices (or instantaneous events).
        renderer: SliceTrack.create({
          trace,
          uri,
          // SourceDataset tells the SliceTrack where to fetch its data from.
          dataset: new SourceDataset({
            src: EVENT_TABLE_NAME, // Our custom table created earlier.
            schema: {
              ts: LONG,
              dur: LONG,
              type: STR,
              id: NUM,
              reference_ts: LONG,
            },
            // Only fetch events for this specific process.
            filter: { col: 'upid', eq: upid },
          }),
          // This callback defines what happens when the user clicks on the event in the track.
          detailsPanel: (row) => {
            const ts = Time.fromRaw(row.reference_ts);
            // Return our custom details panel to show the flamegraph.
            return new OomCallstackDetailsPanel(
              trace,
              upid,
              ts,
              store.state.trackFlamegraphState,
              // Persist the state whenever the user interacts with the flamegraph.
              (state) => {
                store.edit((draft) => {
                  draft.trackFlamegraphState = state;
                });
              },
            );
          },
          tooltip: () => 'OOM Callstack',
          // Colorize the slice based on its timestamp to keep it consistent.
          colorizer: (slice) => materialColorScheme(slice.ts.toString()),
        }),
      };

      // Register the track globally in the trace.
      trace.tracks.registerTrack(track);

      // Create a node for the workspace tree to visually display the track under the process group.
      const trackNode = new TrackNode({
        uri,
        name: 'OOM callstack',
        sortOrder: -30, // Push it towards the top of the process group.
      });

      group.addChildInOrder(trackNode);
    }
  }


}
