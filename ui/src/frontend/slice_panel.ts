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
            `Slice Details:`),
          m('.slice-details-table', [
            m('table', [
              m('tr',
                m('td',`PID`),
                m('td',`${threadInfo.pid}`)),
              m('tr',
                m('td',`Process name`),
                m('td',`${threadInfo.procName}`)),
              m('tr',
                m('td',`TID`),
                m('td',`${threadInfo.tid}`)),
              m('tr',
                m('td',`Thread name`),
                m('td',`${threadInfo.threadName}`)),
              m('tr',
                m('td',`Start time`),
                m('td',`${sliceInfo.ts} s`)),
              m('tr',
                m('td',`Duration`),
                m('td',`${sliceInfo.dur} s`)),
              m('tr',
                m('td',`Prio`),
                m('td',`${sliceInfo.priority}`)),
              m('tr',
                m('td',`End State`),
                m('td',`${sliceInfo.endState}`))
            ])],
          ));
    }
  else {
    return m(
      '.slice-details-panel',
      m('.slice-details-panel-heading',
        `Slice Details:`,
      ));
  }
}
  renderCanvas(_ctx: CanvasRenderingContext2D, _size: PanelSize) {}
}