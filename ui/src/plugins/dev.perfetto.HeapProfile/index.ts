// Copyright (C) 2021 The Android Open Source Project
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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {NUM, STR} from '../../trace_processor/query_result';
import {createHeapProfileTrack} from './heap_profile_track';
import {TrackNode} from '../../public/workspace';
import {
  createPerfettoTable,
  createPerfettoView,
} from '../../trace_processor/sql_utils';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {Track} from '../../public/track';
import {FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';
import {Store} from '../../base/store';
import {z} from 'zod';
import {assertExists} from '../../base/assert';
import {profileDescriptor} from './common';

const EVENT_TABLE_NAME = 'heap_profile_events';

const HEAP_PROFILE_PLUGIN_STATE_SCHEMA = z.object({
  detailsPanelFlamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
});

type HeapProfilePluginState = z.infer<typeof HEAP_PROFILE_PLUGIN_STATE_SCHEMA>;

function trackUri(upid: number, type: string): string {
  return `/process_${upid}/${type}_heap_profile`;
}

export default class HeapProfilePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.HeapProfile';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  private readonly trackMap = new Map<string, Track>();
  private store?: Store<HeapProfilePluginState>;

  private migrateHeapProfilePluginState(init: unknown): HeapProfilePluginState {
    const result = HEAP_PROFILE_PLUGIN_STATE_SCHEMA.safeParse(init);
    return result.data ?? {};
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    this.store = trace.mountStore(HeapProfilePlugin.id, (init) =>
      this.migrateHeapProfilePluginState(init),
    );
    await this.createHeapProfileTable(trace);
    await this.addProcessTracks(trace);

    trace.onTraceReady.addListener(async () => {
      await this.selectFirstHeapProfile(trace);
    });
  }

  private async createHeapProfileTable(trace: Trace) {
    await createPerfettoTable({
      engine: trace.engine,
      name: EVENT_TABLE_NAME,
      as: `
        SELECT
          MIN(id) as id,
          graph_sample_ts AS ts,
          upid,
          0 AS dur,
          0 AS depth,
          'java_heap_graph' AS type
        FROM heap_graph_object
        GROUP BY graph_sample_ts, upid

        UNION ALL

        SELECT
          MIN(id) as id,
          ts,
          upid,
          0 AS dur,
          0 AS depth,
          'heap_profile:' || heap_name AS type
        FROM heap_profile_allocation
        GROUP BY ts, upid, heap_name
      `,
    });
  }

  private async addProcessTracks(trace: Trace) {
    const trackGroupsPlugin = trace.plugins.getPlugin(
      ProcessThreadGroupsPlugin,
    );
    const incomplete = await this.getIncomplete(trace);
    const heapTypesResult = await trace.engine.query(`
      SELECT DISTINCT type
      FROM ${EVENT_TABLE_NAME}
    `);
    const heapTypes = [];
    for (const it = heapTypesResult.iter({type: STR}); it.valid(); it.next()) {
      heapTypes.push(it.type);
    }

    let typeIdx = 0;
    for (const heapType of heapTypes) {
      // Create a view for this particular type
      const viewName = `${EVENT_TABLE_NAME}_view_${typeIdx}`;
      await createPerfettoView({
        engine: trace.engine,
        name: viewName,
        as: `
          SELECT *
          FROM ${EVENT_TABLE_NAME}
          WHERE type = '${heapType}'
        `,
      });
      typeIdx++;

      const upidResult = await trace.engine.query(`
        SELECT DISTINCT upid
        FROM ${viewName}
      `);

      const upids = [];
      for (const it = upidResult.iter({upid: NUM}); it.valid(); it.next()) {
        upids.push(it.upid);
      }

      for (const upid of upids) {
        const group = trackGroupsPlugin.getGroupForProcess(upid);
        if (!group) continue;

        const store = assertExists(this.store);
        const uri = trackUri(upid, heapType);

        const track: Track = {
          uri,
          tags: {
            upid,
          },
          renderer: createHeapProfileTrack(
            trace,
            uri,
            viewName,
            upid,
            incomplete,
            store.state.detailsPanelFlamegraphState,
            (state) => {
              store.edit((draft) => {
                draft.detailsPanelFlamegraphState = state;
              });
            },
          ),
        };

        trace.tracks.registerTrack(track);
        this.trackMap.set(uri, track);

        const trackNode = new TrackNode({
          uri,
          name: profileDescriptor(heapType).label,
          sortOrder: -30,
        });
        group.addChildInOrder(trackNode);
      }
    }
  }

  private async getIncomplete(trace: Trace): Promise<boolean> {
    const it = await trace.engine.query(`
      SELECT value
      FROM stats
      WHERE name = 'heap_graph_non_finalized_graph'
    `);
    const incomplete = it.firstRow({value: NUM}).value > 0;
    return incomplete;
  }

  private async selectFirstHeapProfile(ctx: Trace) {
    const result = await ctx.engine.query(`
        SELECT
          id,
          upid,
          type
        FROM ${EVENT_TABLE_NAME}
        ORDER BY ts
        LIMIT 1
      `);

    const iter = result.maybeFirstRow({id: NUM, upid: NUM, type: STR});
    if (!iter) return;

    const track = this.trackMap.get(trackUri(iter.upid, iter.type));
    if (!track) return;

    ctx.selection.selectTrackEvent(track.uri, iter.id);
  }
}
