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

import {timeToCode} from '../common/time';

import {globals} from './globals';
import {Panel} from './panel';

interface HeapDumpDetailsPanelAttrs {}

export class HeapDumpDetailsPanel extends Panel<HeapDumpDetailsPanelAttrs> {
  view() {
    const heapDumpInfo = globals.heapDumpDetails;
    if (heapDumpInfo && heapDumpInfo.ts && heapDumpInfo.allocated &&
        heapDumpInfo.allocatedNotFreed) {
      return m(
          '.details-panel',
          m('.details-panel-heading', `Heap Snapshot Details:`),
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
              ));
    } else {
      return m(
          '.details-panel',
          m('.details-panel-heading', `Heap Snapshot Details:`));
    }
  }

  renderCanvas() {}
}
