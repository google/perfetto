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
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import {asSchedSqlId} from '../../components/sql_utils/core_types';
import {
  getSched,
  getSchedWakeupInfo,
  Sched,
  SchedWakeupInfo,
} from '../../components/sql_utils/sched';
import {exists} from '../../base/utils';
import {translateState} from '../../components/sql_utils/thread_state';
import {Trace} from '../../public/trace';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {ThreadDesc, ThreadMap} from '../dev.perfetto.Thread/threads';

const MIN_NORMAL_SCHED_PRIORITY = 100;

function getDisplayName(
  name: string | undefined,
  id: bigint | number | undefined,
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

  constructor(
    private readonly trace: Trace,
    private readonly threads: ThreadMap,
  ) {}

  async load({eventId}: TrackEventSelection) {
    const sched = await getSched(this.trace.engine, asSchedSqlId(eventId));
    if (sched === undefined) {
      return;
    }
    const wakeup = await getSchedWakeupInfo(this.trace.engine, sched);
    this.details = {sched, wakeup};
  }

  render() {
    if (this.details === undefined) {
      return m(DetailsShell, {title: 'Sched', description: 'Loading...'});
    }
    const threadInfo = this.threads.get(this.details.sched.thread.utid);

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
    const threadInfo = this.threads.get(data.sched.thread.utid);
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

    const svgString = `
      <svg class="pf-sched-latency__background" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 300" width="200" height="300">
        <line x1="40" y1="20" x2="40" y2="280" stroke="currentColor" stroke-width="3"/>
        <polygon points="40,65 50,80 40,95 30,80" fill="currentColor"/>
        <line x1="40" y1="200" x2="180" y2="200" stroke="currentColor" stroke-width="3"/>
        <polygon points="40,200 52,193 52,207" fill="currentColor"/>
        <polygon points="180,200 168,193 168,207" fill="currentColor"/>
      </svg>
    `;

    return m(
      Section,
      {title: 'Scheduling Latency'},
      m(
        '.pf-sched-latency',
        m.trust(svgString),
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
    const threadInfo = this.threads.get(data.wakeup.wakerUtid);
    if (!threadInfo) {
      return null;
    }
    return m(
      '.pf-sched-latency__wakeup-text',
      m(
        '',
        `Wakeup @ `,
        m(Timestamp, {trace: this.trace, ts: data.wakeup?.wakeupTs}),
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
      '.pf-sched-latency__latency-text',
      m(
        '',
        `Scheduling latency: `,
        m(DurationWidget, {trace: this.trace, dur: latency}),
      ),
      m(
        '.pf-sched-latency__explanation',
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
        right: m(Timestamp, {trace: this.trace, ts: data.sched.ts}),
      }),
      m(TreeNode, {
        left: 'Duration',
        right: m(DurationWidget, {trace: this.trace, dur: data.sched.dur}),
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
    if (data.sched.threadStateId) {
      this.trace.selection.selectSqlEvent(
        'thread_state',
        data.sched.threadStateId,
        {scrollToSelection: true},
      );
    }
  }

  renderCanvas() {}
}
