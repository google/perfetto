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

import * as m from 'mithril';

import {Actions} from '../common/actions';
import {timeToCode, toNs} from '../common/time';

import {globals} from './globals';
import {Panel, PanelSize} from './panel';
import {scrollToTrackAndTs} from './scroll_helper';


export class ThreadStatePanel extends Panel {
  view() {
    const threadState = globals.threadStateDetails;
    if (threadState === undefined || threadState.utid === undefined ||
        threadState.ts === undefined || threadState.dur === undefined ||
        threadState.state === undefined) {
      return m('.details-panel');
    }
    const threadInfo = globals.threads.get(threadState.utid);
    if (threadInfo) {
      return m(
          '.details-panel',
          m('.details-panel-heading', m('h2', 'Thread State')),
          m('.details-table', [m('table.half-width', [
              m('tr',
                m('th', `Start time`),
                m('td', `${timeToCode(threadState.ts)}`)),
              m('tr',
                m('th', `Duration`),
                m(
                    'td',
                    `${timeToCode(threadState.dur)} `,
                    )),
              m('tr',
                m('th', `State`),
                m('td',
                  this.getStateContent(
                      threadState.state,
                      threadState.cpu,
                      threadState.sliceId,
                      threadState.ts))),
              m('tr',
                m('th', `Process`),
                m('td', `${threadInfo.procName} [${threadInfo.pid}]`)),
            ])]));
    }
    return m('.details-panel');
  }

  renderCanvas(_ctx: CanvasRenderingContext2D, _size: PanelSize) {}

  // If it is the running state, we want to show which CPU and a button to
  // go to the sched slice. Otherwise, just show the state.
  getStateContent(
      state: string, cpu: number|undefined, sliceId: number|undefined,
      ts: number) {
    if (sliceId === undefined || cpu === undefined) {
      return [state];
    }

    return [
      `${state} on CPU ${cpu}`,
      m('i.material-icons.grey',
        {
          onclick: () => {
              // TODO(taylori): Use trackId from TP.
              let trackId;
              for (const track of Object.values(globals.state.tracks)) {
                if (track.kind === 'CpuSliceTrack' &&
                    (track.config as {cpu: number}).cpu === cpu) {
                  trackId = track.id;
                }
              }
              if (trackId) {
                globals.makeSelection(
                    Actions.selectSlice({id: sliceId, trackId}));
                scrollToTrackAndTs(
                    trackId, toNs(ts + globals.state.traceTime.startSec));
              }
          },
          title: 'Go to CPU slice'
        },
        'call_made')
    ];
  }
}
