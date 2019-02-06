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
import {globals} from './globals';
import {Panel, PanelSize} from './panel';

interface SliceDetailsPanelAttrs {
  utid: number;
}

export class SliceDetailsPanel extends Panel<SliceDetailsPanelAttrs> {
  view({attrs}: m.CVnode<SliceDetailsPanelAttrs>) {
    const threadInfo = globals.threads.get(attrs.utid);
    const sliceInfo = globals.sliceDetails;
    if (threadInfo && sliceInfo.ts && sliceInfo.dur) {
      return m(
          '.slice-details-panel',
          m('.slice-details-panel-heading',
            `Details for slice:`),
          m('.slice-details-ul', [
            m('ul', [
              m('li', `PID: ${threadInfo.pid}`),
              m('li', `Process name: ${threadInfo.procName}`),
              m('li', `TID: ${threadInfo.tid}`),
              m('li', `Thread name: ${threadInfo.threadName}`),
              m('li', `Start time: ${sliceInfo.ts} s`),
              m('li', `Duration: ${sliceInfo.dur} s`),
              m('li', `Prio: ${sliceInfo.priority}`),
              m('li', `End State: ${sliceInfo.endState}`),
            ])],
          ));
    }
  else {
    return m(
      '.slice-details-panel',
      m('.slice-details-panel-heading',
        `Details for slice: Unavailable`,
      ));
  }
}
  renderCanvas(_ctx: CanvasRenderingContext2D, _size: PanelSize) {}
}