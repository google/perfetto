// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {STR} from '../../trace_processor/query_result';
import {ThreadSortOrder} from '../../lynx_perf/thread_order';

/**
 * Perfetto plugin to adjust thread group display order:
 * - Main thread, Lynx_JS thread, and other Lynx threads are prioritized to the top.
 * - Thread order is controlled via ThreadSortOrder.
 */
export default class LynxThreadGroupPlugin implements PerfettoPlugin {
  static readonly id = 'lynx.ThreadGroup';
  private isLinuxTrace = true;
  /**
   * On trace load, reorders process/thread groups, setting proper sortOrder for each thread.
   */
  async onTraceLoad(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(
      `select str_value from metadata where name = 'system_name'`,
    );
    if (result.numRows() !== 0) {
      const name = result.firstRow({str_value: STR}).str_value;
      if (name !== 'Linux') {
        this.isLinuxTrace = false;
      }
    }
    const children = ctx.workspace.children.slice();
    ctx.workspace.clear();
    // Filter out process groups
    const processGroup = children.filter((child) =>
      child.uri?.startsWith('/process'),
    );
    processGroup.forEach((group) => {
      const pids = group.title.split(' ');
      // default expand process group
      group.expand();
      const pid = parseInt(pids[pids.length - 1]);
      const threadGroup = group.children.slice();
      group.clear();
      // Extract and sort thread nodes by TID (Thread ID)
      const sortedThread = threadGroup
        .filter((thread) => thread.uri?.startsWith('/thread'))
        .sort((a, b) => {
          const aTids = a.title.split(' ');
          const aTid = parseInt(aTids[aTids.length - 1]);
          const bTids = b.title.split(' ');
          const bTid = parseInt(bTids[bTids.length - 1]);
          return aTid - bTid;
        });
      // Assign sortOrder for each thread
      for (let i = 0; i < sortedThread.length; i++) {
        const thread = sortedThread[i];
        // For non-Linux traces, designate the first (lowest TID) thread as the main thread
        if (i === 0 && !this.isLinuxTrace) {
          thread.sortOrder = ThreadSortOrder.MAIN_THREAD;
        } else {
          const tids = thread.title.split(' ');
          const tid = parseInt(tids[tids.length - 1]);
          const threadName = tids.slice(0, tids.length - 1).join(' ');
          thread.sortOrder = this.getThreadSortOrder(pid, tid, threadName);
        }
      }
      threadGroup.forEach((thread) => {
        group.addChildInOrder(thread);
      });
    });
    children.forEach((child) => {
      ctx.workspace.addChildInOrder(child);
    });
  }

  /**
   * Determine the sortOrder for a thread based on PID, TID, and thread name.
   * @param pid Process ID.
   * @param tid Thread ID.
   * @param threadName Thread name.
   * @returns Corresponding Thread sortOrder value.
   */
  private getThreadSortOrder(pid: number, tid: number, threadName: string) {
    // Main thread: TID equals PID
    if (pid === tid) {
      return ThreadSortOrder.MAIN_THREAD;
    }
    // Lynx background thread: thread name starts with 'Lynx_JS'
    if (threadName.startsWith('Lynx_JS')) {
      return ThreadSortOrder.LYNX_BACKGROUND_THREAD;
    }
    // Other Lynx threads: thread name starts with 'Lynx' or 'lynx_'
    if (threadName.startsWith('Lynx') || threadName.startsWith('lynx_')) {
      return ThreadSortOrder.LYNX_THREAD;
    }
    // Default to other threads
    return ThreadSortOrder.OTHER_THREAD;
  }
}
