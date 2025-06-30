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
import {Trace} from '../public/trace';
import {AggregateData, BarChartData, Column} from './aggregation';
import {AggregationPanelAttrs, AggState} from './aggregation_adapter';
import {translateState} from './sql_utils/thread_state';
import {DurationWidget} from './widgets/duration';

export class AggregationPanel
  implements m.ClassComponent<AggregationPanelAttrs>
{
  private trace: Trace;

  constructor({attrs}: m.CVnode<AggregationPanelAttrs>) {
    this.trace = attrs.trace;
  }

  view({attrs}: m.CVnode<AggregationPanelAttrs>) {
    return m(
      '.details-panel',
      m(
        '.details-panel-heading.aggregation',
        attrs.data.barChart !== undefined &&
          this.renderBarChart(attrs.data.barChart),
        this.showTimeRange(),
        m(
          'table',
          m(
            'tr',
            attrs.data.columns.map((col) =>
              this.formatColumnHeading(col, attrs.model),
            ),
          ),
          m(
            'tr.sum',
            attrs.data.columnSums.map((sum) => {
              const sumClass = sum === '' ? 'td' : 'td.sum-data';
              return m(sumClass, sum);
            }),
          ),
        ),
      ),
      m('.details-table.aggregation', m('table', this.getRows(attrs.data))),
    );
  }

  formatColumnHeading(col: Column, model: AggState) {
    const pref = model.getSortingPrefs();
    let sortIcon = '';
    if (pref && pref.column === col.columnId) {
      sortIcon =
        pref.direction === 'DESC' ? 'arrow_drop_down' : 'arrow_drop_up';
    }
    return m(
      'th',
      {
        onclick: () => {
          model.toggleSortingColumn(col.columnId);
        },
      },
      col.title,
      m('i.material-icons', sortIcon),
    );
  }

  getRows(data: AggregateData) {
    if (data.columns.length === 0) return;
    const rows = [];
    for (let i = 0; i < data.columns[0].data.length; i++) {
      const row = [];
      for (let j = 0; j < data.columns.length; j++) {
        row.push(m('td', this.getFormattedData(data, i, j)));
      }
      rows.push(m('tr', row));
    }
    return rows;
  }

  getFormattedData(data: AggregateData, rowIndex: number, columnIndex: number) {
    switch (data.columns[columnIndex].kind) {
      case 'STRING':
        return data.strings[data.columns[columnIndex].data[rowIndex]];
      case 'TIMESTAMP_NS':
        return `${data.columns[columnIndex].data[rowIndex] / 1000000}`;
      case 'STATE': {
        const concatState =
          data.strings[data.columns[columnIndex].data[rowIndex]];
        const split = concatState.split(',');
        const ioWait =
          split[1] === 'NULL' ? undefined : !!Number.parseInt(split[1], 10);
        return translateState(split[0], ioWait);
      }
      case 'NUMBER':
      default:
        return data.columns[columnIndex].data[rowIndex];
    }
  }

  showTimeRange() {
    const selection = this.trace.selection.selection;
    if (selection.kind !== 'area') return undefined;
    const duration = selection.end - selection.start;
    return m(
      '.time-range',
      'Selected range: ',
      m(DurationWidget, {dur: duration}),
    );
  }

  renderBarChart(data: ReadonlyArray<BarChartData>) {
    const totalTime = data.reduce((sum, item) => sum + item.timeInStateMs, 0);
    return m(
      '.states',
      data.map((d) => {
        const width = (d.timeInStateMs / totalTime) * 100;
        return m(
          '.state',
          {
            style: {
              background: d.color.base.cssString,
              color: d.color.textBase.cssString,
              width: `${width}%`,
            },
          },
          `${d.name}: ${d.timeInStateMs} ms`,
        );
      }),
    );
  }
}
