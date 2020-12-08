// Copyright (C) 2020 The Android Open Source Project
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

import {CallsiteInfo} from '../common/state';
import {globals} from './globals';
import {Panel} from './panel';

interface CpuProfileDetailsPanelAttrs {}

export class CpuProfileDetailsPanel extends Panel<CpuProfileDetailsPanelAttrs> {
  view() {
    const sampleDetails = globals.cpuProfileDetails;
    const header =
        m('.details-panel-heading', m('h2', `CPU Profile Sample Details`));
    if (!sampleDetails || sampleDetails.id === undefined) {
      return m('.details-panel', header);
    }

    return m(
        '.details-panel',
        header,
        m('table', this.getStackText(sampleDetails.stack)));
  }

  getStackText(stack?: CallsiteInfo[]): m.Vnode[] {
    if (!stack) return [];

    const result = [];
    for (let i = 0; i < stack.length; i++) {
      result.push(m('tr', m('td', stack[i].name), m('td', stack[i].mapping)));
    }

    return result;
  }

  renderCanvas() {}
}
