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
import {translateState} from '../common/thread_state';
import {tpTimeToCode} from '../common/time';
import {globals, SliceDetails, ThreadDesc} from './globals';
import {scrollToTrackAndTs} from './scroll_helper';
import {SlicePanel} from './slice_panel';

export class SliceDetailsPanel extends SlicePanel {
  view() {
    const sliceInfo = globals.sliceDetails;
    if (sliceInfo.utid === undefined) return;
    const threadInfo = globals.threads.get(sliceInfo.utid);

    return m(
        '.details-panel',
        m(
            '.details-panel-heading',
            m('h2.split', `Slice Details`),
            this.hasSchedLatencyInfo(sliceInfo) &&
                m('h2.split', 'Scheduling Latency'),
            ),
        this.renderDetails(sliceInfo, threadInfo));
  }

  private renderSchedLatencyInfo(sliceInfo: SliceDetails): m.Children {
    if (!this.hasSchedLatencyInfo(sliceInfo)) {
      return null;
    }
    return m(
        '.half-width-panel.slice-details-latency-panel',
        m('img.slice-details-image', {
          src: `${globals.root}assets/scheduling_latency.png`,
        }),
        this.renderWakeupText(sliceInfo),
        this.renderDisplayLatencyText(sliceInfo),
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
    const timestamp =
        tpTimeToCode(sliceInfo.wakeupTs! - globals.state.traceTime.start);
    return m(
        '.slice-details-wakeup-text',
        m('', `Wakeup @ ${timestamp} on CPU ${sliceInfo.wakerCpu} by`),
        m('', `P: ${threadInfo.procName} [${threadInfo.pid}]`),
        m('', `T: ${threadInfo.threadName} [${threadInfo.tid}]`),
    );
  }

  private renderDisplayLatencyText(sliceInfo: SliceDetails): m.Children {
    if (sliceInfo.ts === undefined || sliceInfo.wakeupTs === undefined) {
      return null;
    }

    const latency = tpTimeToCode(sliceInfo.ts - sliceInfo.wakeupTs);
    return m(
        '.slice-details-latency-text',
        m('', `Scheduling latency: ${latency}`),
        m('.text-detail',
          `This is the interval from when the task became eligible to run
        (e.g. because of notifying a wait queue it was suspended on) to
        when it started running.`),
    );
  }

  private hasSchedLatencyInfo({wakeupTs, wakerUtid}: SliceDetails): boolean {
    return wakeupTs !== undefined && wakerUtid !== undefined;
  }

  private renderDetails(sliceInfo: SliceDetails, threadInfo?: ThreadDesc):
      m.Children {
    if (!threadInfo || sliceInfo.ts === undefined ||
        sliceInfo.dur === undefined) {
      return null;
    } else {
      const tableRows = [
        m('tr',
          m('th', `Process`),
          m('td', `${threadInfo.procName} [${threadInfo.pid}]`)),
        m('tr',
          m('th', `Thread`),
          m('td',
            `${threadInfo.threadName} [${threadInfo.tid}]`,
            m('i.material-icons.grey',
              {onclick: () => this.goToThread(), title: 'Go to thread'},
              'call_made'))),
        m('tr', m('th', `Cmdline`), m('td', threadInfo.cmdline)),
        m('tr',
          m('th', `Start time`),
          m('td',
            `${tpTimeToCode(sliceInfo.ts - globals.state.traceTime.start)}`)),
        m('tr',
          m('th', `Duration`),
          m('td', this.computeDuration(sliceInfo.ts, sliceInfo.dur))),
        (sliceInfo.threadDur === undefined ||
         sliceInfo.threadTs === undefined) ?
            '' :
            m('tr',
              m('th', 'Thread duration'),
              m('td',
                this.computeDuration(sliceInfo.threadTs, sliceInfo.threadDur))),
        m('tr', m('th', `Prio`), m('td', `${sliceInfo.priority}`)),
        m('tr',
          m('th', `End State`),
          m('td', translateState(sliceInfo.endState))),
        m('tr',
          m('th', `Slice ID`),
          m('td',
            (sliceInfo.id !== undefined) ? sliceInfo.id.toString() :
                                           'Unknown')),
      ];

      for (const [key, value] of this.getProcessThreadDetails(sliceInfo)) {
        if (value !== undefined) {
          tableRows.push(m('tr', m('th', key), m('td', value)));
        }
      }

      return m(
          '.details-table-multicolumn',
          m('table.half-width-panel', tableRows),
          this.renderSchedLatencyInfo(sliceInfo),
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

    let trackId: string|number|undefined;
    for (const track of Object.values(globals.state.tracks)) {
      if (track.kind === 'ThreadStateTrack' &&
          (track.config as {utid: number}).utid === threadInfo.utid) {
        trackId = track.id;
      }
    }

    if (trackId && sliceInfo.threadStateId) {
      globals.makeSelection(Actions.selectThreadState({
        id: sliceInfo.threadStateId,
        trackId: trackId.toString(),
      }));

      scrollToTrackAndTs(trackId, sliceInfo.ts, true);
    }
  }

  renderCanvas() {}
}
