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
import {timeToCode} from '../common/time';

import {globals} from './globals';
import {Panel} from './panel';

interface HeapProfileDetailsPanelAttrs {}

export class HeapProfileDetailsPanel extends
    Panel<HeapProfileDetailsPanelAttrs> {
  private ts = 0;
  private pid = 0;

  view() {
    const heapDumpInfo = globals.heapDumpDetails;
    if (heapDumpInfo && heapDumpInfo.ts !== undefined &&
        heapDumpInfo.allocated !== undefined &&
        heapDumpInfo.allocatedNotFreed !== undefined &&
        heapDumpInfo.tsNs !== undefined && heapDumpInfo.pid !== undefined) {
      this.ts = heapDumpInfo.tsNs;
      this.pid = heapDumpInfo.pid;
      return m(
          '.details-panel',
          m('.details-panel-heading', `Heap Profile Details:`),
          m(
              '.details-table',
              [m('table',
                 [
                   m('tr',
                     m('th', `Snapshot time`),
                     m('td', `${timeToCode(heapDumpInfo.ts)}`)),
                   m('tr',
                     m('th', `Total allocated:`),
                     m('td',
                       `${heapDumpInfo.allocated.toLocaleString()} bytes`)),
                   m('tr',
                     m('th', `Allocated not freed:`),
                     m('td',
                       `${
                           heapDumpInfo.allocatedNotFreed
                               .toLocaleString()} bytes`)),
                 ])],
              ),
          m('.explanation',
            'Heap profile support is in beta. If you need missing features, ',
            'download and open it in ',
            m(`a[href='https://pprof.corp.google.com']`, 'pprof'),
            ' (Googlers only) or ',
            m(`a[href='https://www.speedscope.app']`, 'Speedscope'),
            '.'),
          m('button',
            {
              onclick: () => {
                this.downloadPprof();
              }
            },
            m('i.material-icons', 'file_download'),
            'Download profile'),
      );
    } else {
      return m(
          '.details-panel',
          m('.details-panel-heading', `Heap Snapshot Details:`));
    }
  }

  downloadPprof() {
    const engine = Object.values(globals.state.engines)[0];
    if (!engine) return;
    const src = engine.source;
    // TODO(tneda): add second timestamp
    globals.dispatch(
        Actions.convertTraceToPprof({pid: this.pid, ts1: this.ts, src}));
  }

  renderCanvas() {}
}
