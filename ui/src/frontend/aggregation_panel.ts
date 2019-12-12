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

import {AggregateCpuData} from '../common/aggregation_data';

import {globals} from './globals';
import {Panel} from './panel';

export class AggregationPanel extends Panel {
  view() {
    const data = globals.aggregateCpuData;
    return m(
        '.details-panel',
        m('.details-panel-heading.aggregation',
          m('table',
            m('tr',
              m('th', 'Process'),
              m('th', 'Thread'),
              m('th', 'Wall duration (ms)'),
              m('th', 'Avg. Wall duration (ms)'),
              m('th', 'Occurrences')))),
        m(
            '.details-table.aggregation',
            m('table', this.getRows(data)),
            ));
  }

  getRows(data: AggregateCpuData) {
    if (!data.strings || !data.procNameId || !data.threadNameId || !data.pid ||
        !data.tid || !data.totalDur || !data.occurrences) {
      return;
    }
    const rows = [];
    for (let i = 0; i < data.pid.length; i++) {
      const row =
          [m('tr',
             m('td', `${data.strings[data.procNameId[i]]} [${data.pid[i]}]`),
             m('td', `${data.strings[data.threadNameId[i]]} [${data.tid[i]}]`),
             m('td', `${data.totalDur[i] / 1000000}`),
             m('td',
               `${
                   +
                   (data.totalDur[i] / data.occurrences[i] / 1000000)
                       .toFixed(6)}`),
             m('td', `${data.occurrences[i]}`))];
      rows.push(row);
    }
    return rows;
  }

  renderCanvas() {}
}
