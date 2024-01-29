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

import {
  NUM,
  NUM_NULL,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  STR,
} from '../../public';
import {addDebugSliceTrack} from '../../public';

class AndroidClientServer implements Plugin {
  onActivate(_: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerCommand({
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
            slice.id,
            slice.ts,
            slice.dur,
            coalesce(req.aidl_name, rep.aidl_name, slice.name) name,
            tt.utid,
            thread.upid resolved_upid,
            case
              when req.binder_txn_id is not null then 'request'
              when rep.binder_reply_id is not null then 'response'
              else 'slice'
            end as slice_type,
            coalesce(req.is_sync, rep.is_sync, true) as is_sync
          from graph_reachable_dfs!(
            all_binder_txns_considered,
            (select id from s)
          ) dfs
          join slice on dfs.node_id = slice.id
          join thread_track tt on slice.track_id = tt.id
          join thread using (utid)
          -- TODO(lalitm): investigate whether it is worth improve this.
          left join android_binder_txns req on slice.id = req.binder_txn_id
          left join android_binder_txns rep on slice.id = rep.binder_reply_id
          where resolved_upid is not null;
        `);
        await ctx.engine.query(`
          create or replace perfetto table __thread_state_for_${sliceId} as
          with foo as (
            select
              ii.ts,
              ii.dur,
              tstate.utid,
              thread.upid,
              tstate.state,
              tstate.io_wait,
              (
                select name
                from thread_slice tslice
                where tslice.utid = tstate.utid and tslice.ts < ii.ts
                order by ts desc
                limit 1
              ) as enclosing_slice_name
            from interval_intersect!(
              (
                select id, ts, dur
                from __binder_for_slice_${sliceId}
                where slice_type IN ('slice', 'response') and is_sync
              ),
              (
                select id, ts, dur
                from thread_state tstate
                where tstate.utid in (
                  select distinct utid
                  from __binder_for_slice_${sliceId}
                  where slice_type IN ('slice', 'response') and is_sync
                )
              )
            ) ii
            join __binder_for_slice_${sliceId} bfs on ii.left_id = bfs.id
            join thread_state tstate on ii.right_id = tstate.id
            join thread using (utid)
            where bfs.utid = tstate.utid
          )
          select *, 
            case
              when state = 'S' and enclosing_slice_name = 'binder transaction' then 'Binder'
              when state = 'S' and enclosing_slice_name GLOB 'Lock*' then 'Lock contention'
              when state = 'S' and enclosing_slice_name GLOB 'Monitor*' then 'Lock contention'
              when state = 'S' then 'Sleeping'
              when state = 'R' then 'Runnable'
              when state = 'Running' then 'Running'
              when state GLOB 'R*' then 'Runnable'
              when state GLOB 'D*' and io_wait then 'IO'
              when state GLOB 'D*' and not io_wait then 'Unint-sleep'
            end as name
          from foo;
        `);

        const res = await ctx.engine.query(`
          select
            process.upid,
            ifnull(process.name, 'Unknown Process') as process_name,
            tstate.upid as tstate_upid
          from (
            select distinct resolved_upid from __binder_for_slice_${sliceId}
          ) binder_for_slice
          join process on binder_for_slice.resolved_upid = process.upid
          left join (
            select distinct upid from __thread_state_for_${sliceId}
          ) tstate using (upid);
        `);
        const it = res.iter({
          upid: NUM,
          process_name: STR,
          tstate_upid: NUM_NULL,
        });
        for (; it.valid(); it.next()) {
          if (it.tstate_upid !== null) {
            await addDebugSliceTrack(
              ctx.engine,
              {
                sqlSource: `
                  SELECT ts, dur, name
                  FROM __thread_state_for_${sliceId}
                  WHERE upid = ${it.upid}
                `,
              },
              it.process_name,
              {ts: 'ts', dur: 'dur', name: 'name'},
              [],
            );
          }
          await addDebugSliceTrack(
            ctx.engine,
            {
              sqlSource: `
                SELECT ts, dur, name
                FROM __binder_for_slice_${sliceId}
                WHERE resolved_upid = ${it.upid}
              `,
            },
            it.process_name,
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
