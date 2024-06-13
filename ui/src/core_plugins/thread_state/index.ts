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

import {uuidv4} from '../../base/uuid';
import {THREAD_STATE_TRACK_KIND} from '../../public';
import {asThreadStateSqlId} from '../../frontend/sql_types';
import {ThreadStateTab} from '../../frontend/thread_state_tab';
import {
  BottomTabToSCSAdapter,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {getTrackName} from '../../public/utils';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';

import {ThreadStateTrack} from './thread_state_track';

class ThreadState implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
      select
        utid,
        upid,
        tid,
        thread.name as threadName
      from thread
      join _sched_summary using (utid)
    `);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const tid = it.tid;
      const threadName = it.threadName;
      const displayName = getTrackName({
        utid,
        tid,
        threadName,
        kind: THREAD_STATE_TRACK_KIND,
      });

      ctx.registerTrack({
        uri: `perfetto.ThreadState#${utid}`,
        displayName,
        kind: THREAD_STATE_TRACK_KIND,
        utid,
        trackFactory: ({trackKey}) => {
          return new ThreadStateTrack(
            {
              engine: ctx.engine,
              trackKey,
            },
            utid,
          );
        },
      });
    }

    ctx.registerDetailsPanel(
      new BottomTabToSCSAdapter({
        tabFactory: (sel) => {
          if (sel.kind !== 'THREAD_STATE') {
            return undefined;
          }
          return new ThreadStateTab({
            config: {
              id: asThreadStateSqlId(sel.id),
            },
            engine: ctx.engine,
            uuid: uuidv4(),
          });
        },
      }),
    );
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.ThreadState',
  plugin: ThreadState,
};
