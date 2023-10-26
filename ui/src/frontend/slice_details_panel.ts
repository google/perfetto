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

import {Actions} from '../common/actions';
import {pluginManager} from '../common/plugins';
import {translateState} from '../common/thread_state';
import {THREAD_STATE_TRACK_KIND} from '../tracks/thread_state';
import {Anchor} from '../widgets/anchor';
import {DetailsShell} from '../widgets/details_shell';
import {DurationWidget} from '../widgets/duration';
import {GridLayout} from '../widgets/grid_layout';
import {Section} from '../widgets/section';
import {SqlRef} from '../widgets/sql_ref';
import {Tree, TreeNode} from '../widgets/tree';

import {globals, SliceDetails, ThreadDesc} from './globals';
import {scrollToTrackAndTs} from './scroll_helper';
import {SlicePanel} from './slice_panel';
import {Timestamp} from './widgets/timestamp';

export class SliceDetailsPanel extends SlicePanel {
  view() {
    const sliceInfo = globals.sliceDetails;
    if (sliceInfo.utid === undefined) return;
    const threadInfo = globals.threads.get(sliceInfo.utid);

    return m(
        DetailsShell,
        {
          title: 'CPU Sched Slice',
          description: this.renderDescription(sliceInfo),
        },
        m(
            GridLayout,
            this.renderDetails(sliceInfo, threadInfo),
            this.renderSchedLatencyInfo(sliceInfo),
            ),
    );
  }

  private renderDescription(sliceInfo: SliceDetails) {
    const threadInfo = globals.threads.get(sliceInfo.wakerUtid!);
    if (!threadInfo) {
      return null;
    }
    return `${threadInfo.procName} [${threadInfo.pid}]`;
  }

  private renderSchedLatencyInfo(sliceInfo: SliceDetails): m.Children {
    if (!this.hasSchedLatencyInfo(sliceInfo)) {
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
            this.renderWakeupText(sliceInfo),
            this.renderDisplayLatencyText(sliceInfo),
            ),
    );
  }

  private renderWakeupText(sliceInfo: SliceDetails): m.Children {
    if (sliceInfo.wakerUtid === undefined) {
      return null;
    }
    const threadInfo = globals.threads.get(sliceInfo.wakerUtid!);
    if (!threadInfo) {
      return null;
    }
    return m(
        '.slice-details-wakeup-text',
        m('',
          `Wakeup @ `,
          m(Timestamp, {ts: sliceInfo.wakeupTs!}),
          ` on CPU ${sliceInfo.wakerCpu} by`),
        m('', `P: ${threadInfo.procName} [${threadInfo.pid}]`),
        m('', `T: ${threadInfo.threadName} [${threadInfo.tid}]`),
    );
  }

  private renderDisplayLatencyText(sliceInfo: SliceDetails): m.Children {
    if (sliceInfo.ts === undefined || sliceInfo.wakeupTs === undefined) {
      return null;
    }

    const latency = sliceInfo.ts - sliceInfo.wakeupTs;
    return m(
        '.slice-details-latency-text',
        m('', `Scheduling latency: `, m(DurationWidget, {dur: latency})),
        m('.text-detail',
          `This is the interval from when the task became eligible to run
        (e.g. because of notifying a wait queue it was suspended on) to
        when it started running.`),
    );
  }

  private hasSchedLatencyInfo({wakeupTs, wakerUtid}: SliceDetails): boolean {
    return wakeupTs !== undefined && wakerUtid !== undefined;
  }

  private renderThreadDuration(sliceInfo: SliceDetails) {
    if (sliceInfo.threadDur !== undefined && sliceInfo.threadTs !== undefined) {
      return m(TreeNode, {
        icon: 'timer',
        left: 'Thread Duration',
        right: this.computeDuration(sliceInfo.threadTs, sliceInfo.threadDur),
      });
    } else {
      return null;
    }
  }

  private renderDetails(sliceInfo: SliceDetails, threadInfo?: ThreadDesc):
      m.Children {
    if (!threadInfo || sliceInfo.ts === undefined ||
        sliceInfo.dur === undefined) {
      return null;
    } else {
      const extras: m.Children = [];

      for (const [key, value] of this.getProcessThreadDetails(sliceInfo)) {
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
          right:
              m(Anchor,
                {
                  icon: 'call_made',
                  onclick: () => {
                    this.goToThread();
                  },
                },
                `${threadInfo.threadName} [${threadInfo.tid}]`),
        }),
        m(TreeNode, {
          left: 'Cmdline',
          right: threadInfo.cmdline,
        }),
        m(TreeNode, {
          left: 'Start time',
          right: m(Timestamp, {ts: sliceInfo.ts}),
        }),
        m(TreeNode, {
          left: 'Duration',
          right: this.computeDuration(sliceInfo.ts, sliceInfo.dur),
        }),
        this.renderThreadDuration(sliceInfo),
        m(TreeNode, {
          left: 'Prio',
          right: sliceInfo.priority,
        }),
        m(TreeNode, {
          left: 'End State',
          right: translateState(sliceInfo.endState),
        }),
        m(TreeNode, {
          left: 'SQL ID',
          right: m(SqlRef, {table: 'sched', id: sliceInfo.id}),
        }),
        ...extras,
      ];

      return m(
          Section,
          {title: 'Details'},
          m(Tree, treeNodes),
      );
    }
  }

  goToThread() {
    const sliceInfo = globals.sliceDetails;
    if (sliceInfo.utid === undefined) return;
    const threadInfo = globals.threads.get(sliceInfo.utid);

    if (sliceInfo.id === undefined || sliceInfo.ts === undefined ||
        sliceInfo.dur === undefined || sliceInfo.cpu === undefined ||
        threadInfo === undefined) {
      return;
    }

    let trackKey: string|number|undefined;
    for (const track of Object.values(globals.state.tracks)) {
      const trackDesc = pluginManager.resolveTrackInfo(track.uri);
      // TODO(stevegolton): Handle v2.
      if (trackDesc && trackDesc.kind === THREAD_STATE_TRACK_KIND &&
          trackDesc.utid === threadInfo.utid) {
        trackKey = track.key;
      }
    }

    if (trackKey && sliceInfo.threadStateId) {
      globals.makeSelection(Actions.selectThreadState({
        id: sliceInfo.threadStateId,
        trackKey: trackKey.toString(),
      }));

      scrollToTrackAndTs(trackKey, sliceInfo.ts, true);
    }
  }

  renderCanvas() {}
}
