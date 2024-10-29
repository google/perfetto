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

import {THREAD_STATE_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {getThreadUriPrefix, getTrackName} from '../../public/utils';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {ThreadStateTrack} from './thread_state_track';
import {removeFalsyValues} from '../../base/array_utils';
import {getThreadStateTable} from './table';
import {sqlTableRegistry} from '../../frontend/widgets/sql/table/sql_table_registry';
import {TrackNode} from '../../public/workspace';
import {getOrCreateGroupForThread} from '../../public/standard_groups';
import {ThreadStateSelectionAggregator} from './thread_state_selection_aggregator';
import {extensions} from '../../public/lib/extensions';

function uriForThreadStateTrack(upid: number | null, utid: number): string {
  return `${getThreadUriPrefix(upid, utid)}_state`;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ThreadState';
  async onTraceLoad(ctx: Trace): Promise<void> {
    const {engine} = ctx;

    ctx.selection.registerAreaSelectionAggreagtor(
      new ThreadStateSelectionAggregator(),
    );

    const result = await engine.query(`
      include perfetto module viz.threads;
      include perfetto module viz.summary.threads;

      select
        utid,
        t.upid,
        tid,
        t.name as threadName,
        is_main_thread as isMainThread,
        is_kernel_thread as isKernelThread
      from _threads_with_kernel_flag t
      join _sched_summary using (utid)
    `);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
      isMainThread: NUM_NULL,
      isKernelThread: NUM,
    });
    for (; it.valid(); it.next()) {
      const {utid, upid, tid, threadName, isMainThread, isKernelThread} = it;
      const title = getTrackName({
        utid,
        tid,
        threadName,
        kind: THREAD_STATE_TRACK_KIND,
      });

      const uri = uriForThreadStateTrack(upid, utid);
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          kind: THREAD_STATE_TRACK_KIND,
          utid,
          upid: upid ?? undefined,
          ...(isKernelThread === 1 && {kernelThread: true}),
        },
        chips: removeFalsyValues([
          isKernelThread === 0 && isMainThread === 1 && 'main thread',
        ]),
        track: new ThreadStateTrack(
          {
            trace: ctx,
            uri,
          },
          utid,
        ),
      });

      const group = getOrCreateGroupForThread(ctx.workspace, utid);
      const track = new TrackNode({uri, title, sortOrder: 10});
      group.addChildInOrder(track);
    }

    sqlTableRegistry['thread_state'] = getThreadStateTable();
    ctx.commands.registerCommand({
      id: 'perfetto.ShowTable.thread_state',
      name: 'Open table: thread_state',
      callback: () => {
        extensions.addSqlTableTab(ctx, {
          table: getThreadStateTable(),
        });
      },
    });

    ctx.selection.registerSqlSelectionResolver({
      sqlTableName: 'thread_state',
      callback: async (id: number) => {
        const result = await ctx.engine.query(`
          select
            thread_state.utid,
            thread.upid
          from
            thread_state
            join thread on thread_state.utid = thread.id
          where thread_state.id = ${id}
        `);

        const {upid, utid} = result.firstRow({
          upid: NUM_NULL,
          utid: NUM,
        });

        return {
          eventId: id,
          trackUri: uriForThreadStateTrack(upid, utid),
        };
      },
    });
  }
}
