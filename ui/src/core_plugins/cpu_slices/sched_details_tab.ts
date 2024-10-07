// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import {Anchor} from '../../widgets/anchor';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {Tree, TreeNode} from '../../widgets/tree';
import {globals} from '../../frontend/globals';
import {DurationWidget} from '../../frontend/widgets/duration';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {asSchedSqlId} from '../../trace_processor/sql_utils/core_types';
import {scrollTo} from '../../public/scroll_helper';
import {
  getSched,
  getSchedWakeupInfo,
  Sched,
  SchedWakeupInfo,
} from '../../trace_processor/sql_utils/sched';
import {exists} from '../../base/utils';
import {raf} from '../../core/raf_scheduler';
import {translateState} from '../../trace_processor/sql_utils/thread_state';
import {Trace} from '../../public/trace';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {THREAD_STATE_TRACK_KIND} from '../../public/track_kinds';
import {TrackEventSelection} from '../../public/selection';
import {ThreadDesc} from '../../public/threads';

const MIN_NORMAL_SCHED_PRIORITY = 100;

function getDisplayName(
  name: string | undefined,
  id: number | undefined,
): string | undefined {
  if (name === undefined) {
    return id === undefined ? undefined : `${id}`;
  } else {
    return id === undefined ? name : `${name} ${id}`;
  }
}

interface Data {
  sched: Sched;
  wakeup?: SchedWakeupInfo;
}

export class SchedSliceDetailsPanel implements TrackEventDetailsPanel {
  private details?: Data;

  constructor(private readonly trace: Trace) {}

  async load({eventId}: TrackEventSelection) {
    const sched = await getSched(this.trace.engine, asSchedSqlId(eventId));
    if (sched === undefined) {
      return;
    }
    const wakeup = await getSchedWakeupInfo(this.trace.engine, sched);
    this.details = {sched, wakeup};
    raf.scheduleRedraw();
  }

  render() {
    if (this.details === undefined) {
      return m(DetailsShell, {title: 'Sched', description: 'Loading...'});
    }
    const threadInfo = this.trace.threads.get(this.details.sched.thread.utid);

    return m(
      DetailsShell,
      {
        title: 'CPU Sched Slice',
        description: this.renderTitle(this.details),
      },
      m(
        GridLayout,
        this.renderDetails(this.details, threadInfo),
        this.renderSchedLatencyInfo(this.details),
      ),
    );
  }

  private renderTitle(data: Data) {
    const threadInfo = this.trace.threads.get(data.sched.thread.utid);
    if (!threadInfo) {
      return null;
    }
    return `${threadInfo.procName} [${threadInfo.pid}]`;
  }

  private renderSchedLatencyInfo(data: Data): m.Children {
    if (
      data.wakeup?.wakeupTs === undefined ||
      data.wakeup?.wakerUtid === undefined
    ) {
      return null;
    }
    return m(
      Section,
      {title: 'Scheduling Latency'},
      m(
        '.slice-details-latency-panel',
        m('img.slice-details-image', {
          src: `${globals.root}assets/scheduling_latency.png`,
        }),
        this.renderWakeupText(data),
        this.renderDisplayLatencyText(data),
      ),
    );
  }

  private renderWakeupText(data: Data): m.Children {
    if (
      data.wakeup?.wakerUtid === undefined ||
      data.wakeup?.wakeupTs === undefined ||
      data.wakeup?.wakerCpu === undefined
    ) {
      return null;
    }
    const threadInfo = this.trace.threads.get(data.wakeup.wakerUtid);
    if (!threadInfo) {
      return null;
    }
    return m(
      '.slice-details-wakeup-text',
      m(
        '',
        `Wakeup @ `,
        m(Timestamp, {ts: data.wakeup?.wakeupTs}),
        ` on CPU ${data.wakeup.wakerCpu} by`,
      ),
      m('', `P: ${threadInfo.procName} [${threadInfo.pid}]`),
      m('', `T: ${threadInfo.threadName} [${threadInfo.tid}]`),
    );
  }

  private renderDisplayLatencyText(data: Data): m.Children {
    if (data.wakeup?.wakeupTs === undefined) {
      return null;
    }

    const latency = data.sched.ts - data.wakeup?.wakeupTs;
    return m(
      '.slice-details-latency-text',
      m('', `Scheduling latency: `, m(DurationWidget, {dur: latency})),
      m(
        '.text-detail',
        `This is the interval from when the task became eligible to run
        (e.g. because of notifying a wait queue it was suspended on) to
        when it started running.`,
      ),
    );
  }

  private renderPriorityText(priority?: number) {
    if (priority === undefined) {
      return undefined;
    }
    return priority < MIN_NORMAL_SCHED_PRIORITY
      ? `${priority} (real-time)`
      : `${priority}`;
  }

  protected getProcessThreadDetails(data: Data) {
    const process = data.sched.thread.process;
    return new Map<string, string | undefined>([
      ['Thread', getDisplayName(data.sched.thread.name, data.sched.thread.tid)],
      ['Process', getDisplayName(process?.name, process?.pid)],
      ['User ID', exists(process?.uid) ? String(process?.uid) : undefined],
      ['Package name', process?.packageName],
      [
        'Version code',
        process?.versionCode !== undefined
          ? String(process?.versionCode)
          : undefined,
      ],
    ]);
  }

  private renderDetails(data: Data, threadInfo?: ThreadDesc): m.Children {
    if (!threadInfo) {
      return null;
    }

    const extras: m.Children = [];

    for (const [key, value] of this.getProcessThreadDetails(data)) {
      if (value !== undefined) {
        extras.push(m(TreeNode, {left: key, right: value}));
      }
    }

    const treeNodes = [
      m(TreeNode, {
        left: 'Process',
        right: `${threadInfo.procName} [${threadInfo.pid}]`,
      }),
      m(TreeNode, {
        left: 'Thread',
        right: m(
          Anchor,
          {
            icon: 'call_made',
            onclick: () => {
              this.goToThread(data);
            },
          },
          `${threadInfo.threadName} [${threadInfo.tid}]`,
        ),
      }),
      m(TreeNode, {
        left: 'Cmdline',
        right: threadInfo.cmdline,
      }),
      m(TreeNode, {
        left: 'Start time',
        right: m(Timestamp, {ts: data.sched.ts}),
      }),
      m(TreeNode, {
        left: 'Duration',
        right: m(DurationWidget, {dur: data.sched.dur}),
      }),
      m(TreeNode, {
        left: 'Priority',
        right: this.renderPriorityText(data.sched.priority),
      }),
      m(TreeNode, {
        left: 'End State',
        right: translateState(data.sched.endState),
      }),
      m(TreeNode, {
        left: 'SQL ID',
        right: m(SqlRef, {table: 'sched', id: data.sched.id}),
      }),
      ...extras,
    ];

    return m(Section, {title: 'Details'}, m(Tree, treeNodes));
  }

  goToThread(data: Data) {
    const threadInfo = this.trace.threads.get(data.sched.thread.utid);

    if (threadInfo === undefined) {
      return;
    }

    const trackDescriptor = globals.trackManager.findTrack(
      (td) =>
        td.tags?.kind === THREAD_STATE_TRACK_KIND &&
        td.tags?.utid === threadInfo.utid,
    );

    if (trackDescriptor && data.sched.threadStateId) {
      globals.selectionManager.selectSqlEvent(
        'thread_state',
        data.sched.threadStateId,
      );
      scrollTo({
        track: {uri: trackDescriptor.uri, expandGroup: true},
        time: {start: data.sched.ts},
      });
    }
  }

  renderCanvas() {}
}
