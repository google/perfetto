// Copyright (C) 2023 The Android Open Source Project
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

import {NUM, STR} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {addDebugSliceTrack} from '../../public/debug_tracks';

class AndroidClientServer implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'dev.perfetto.AndroidClientServer#ThreadRuntimeIPC',
      name: 'Show dependencies in client server model',
      callback: async (sliceId) => {
        if (sliceId === undefined) {
          sliceId = prompt('Enter a slice id', '');
          if (sliceId === null) return;
        }
        await ctx.engine.query(`
          include perfetto module android.binder;
          include perfetto module graphs.search;

          create or replace perfetto table __binder_for_slice_${sliceId} as
          with s as materialized (
            select slice.id, ts, ts + dur as ts_end, dur, upid
            from thread_slice slice
            where slice.id = ${sliceId}
          ),
          child_binder_txns_for_slice as materialized (
            select
              (select id from s) as source_node_id,
              binder_txn_id as dest_node_id
            from descendant_slice((select id from s)) as desc
            join android_binder_txns txns on desc.id = txns.binder_txn_id
          ),
          binder_txns_in_slice_intervals as materialized (
            select
              binder_txn_id as source_node_id,
              binder_reply_id as dest_node_id
            from android_binder_txns
            where client_ts > (select ts from s)
              and client_ts < (select ts + dur from s)
          ),
          nested_binder_txns_in_slice_interval as materialized (
            select
              parent.binder_reply_id as source_node_id,
              child.binder_txn_id as dest_node_id
            from android_binder_txns parent
            join descendant_slice(parent.binder_reply_id) desc
            join android_binder_txns child on desc.id = child.binder_txn_id
            where parent.server_ts > (select ts from s)
              and parent.server_ts < (select ts + dur from s)
          ),
          all_binder_txns_considered as materialized (
            select * from child_binder_txns_for_slice
            union
            select * from binder_txns_in_slice_intervals
            union
            select * from nested_binder_txns_in_slice_interval
          )
          select
            dfs.node_id as id,
            coalesce(client.client_ts, server.client_ts, slice.ts) as ts,
            coalesce(client.client_dur, server.client_dur, slice.dur) as dur,
            coalesce(
              client.aidl_name,
              server.aidl_name,
              iif(
                server.binder_reply_id is not null,
                coalesce(
                  server.server_process,
                  server.server_thread,
                  'Unknown server'
                ),
                slice.name
              )
            ) name,
            coalesce(
              client.client_utid,
              server.server_utid,
              thread_track.utid
            ) as utid,
            case
              when client.binder_txn_id is not null then 'client'
              when server.binder_reply_id is not null then 'server'
              else 'slice'
            end as slice_type,
            coalesce(client.is_sync, server.is_sync, true) as is_sync
          from graph_reachable_dfs!(
            all_binder_txns_considered,
            (select id as node_id from s)
          ) dfs
          join slice on dfs.node_id = slice.id
          join thread_track on slice.track_id = thread_track.id
          left join android_binder_txns client on dfs.node_id = client.binder_txn_id
          left join android_binder_txns server on dfs.node_id = server.binder_reply_id
          order by ts;
        `);
        await ctx.engine.query(`
          include perfetto module intervals.intersect;

          create or replace perfetto table __enhanced_binder_for_slice_${sliceId} as
          with foo as (
            select
              bfs.id as binder_id,
              bfs.name as binder_name,
              ii.ts,
              ii.dur,
              tstate.utid,
              thread.upid,
              tstate.cpu,
              tstate.state,
              tstate.io_wait,
              (
                select name
                from thread_slice tslice
                where tslice.utid = tstate.utid and tslice.ts < ii.ts
                order by ts desc
                limit 1
              ) as enclosing_slice_name
            from _interval_intersect!(
              (
                select id, ts, dur
                from __binder_for_slice_${sliceId}
                where slice_type IN ('slice', 'server')
                  and is_sync
                  and dur > 0
              ),
              (
                select id, ts, dur
                from thread_state tstate
                where
                  tstate.utid in (
                    select distinct utid
                    from __binder_for_slice_${sliceId}
                    where
                      slice_type IN ('slice', 'server')
                      and is_sync
                      and dur > 0
                  )
                  and dur > 0
              ),
              ()
            ) ii
            join __binder_for_slice_${sliceId} bfs on ii.id_0 = bfs.id
            join thread_state tstate on ii.id_1 = tstate.id
            join thread using (utid)
            where bfs.utid = tstate.utid
          )
          select
            *,
            case
              when state = 'S' and enclosing_slice_name = 'binder transaction' then 'Waiting for server'
              when state = 'S' and enclosing_slice_name GLOB 'Lock*' then 'Waiting for lock'
              when state = 'S' and enclosing_slice_name GLOB 'Monitor*' then 'Waiting for contention'
              when state = 'S' then 'Sleeping'
              when state = 'R' then 'Waiting for CPU'
              when state = 'Running' then 'Running on CPU ' || foo.cpu
              when state GLOB 'R*' then 'Runnable'
              when state GLOB 'D*' and io_wait then 'IO'
              when state GLOB 'D*' and not io_wait then 'Unint-sleep'
            end as name
          from foo
          order by binder_id;
        `);

        const res = await ctx.engine.query(`
          select id, name
          from __binder_for_slice_${sliceId} bfs
          where slice_type IN ('slice', 'server')
            and dur > 0
          order by ts
        `);
        const it = res.iter({
          id: NUM,
          name: STR,
        });
        for (; it.valid(); it.next()) {
          await addDebugSliceTrack(
            ctx,
            {
              sqlSource: `
                SELECT ts, dur, name
                FROM __enhanced_binder_for_slice_${sliceId}
                WHERE binder_id = ${it.id}
              `,
            },
            it.name,
            {ts: 'ts', dur: 'dur', name: 'name'},
            [],
          );
        }
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.AndroidClientServer',
  plugin: AndroidClientServer,
};
