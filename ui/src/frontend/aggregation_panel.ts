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
import {
  AggregateData,
  Column,
  ThreadStateExtra,
} from '../common/aggregation_data';
import {colorForState, textColorForState} from '../common/colorizer';
import {translateState} from '../common/thread_state';

import {globals} from './globals';
import {Panel} from './panel';

export interface AggregationPanelAttrs {
  data: AggregateData;
  kind: string;
}

export class AggregationPanel extends Panel<AggregationPanelAttrs> {
  view({attrs}: m.CVnode<AggregationPanelAttrs>) {
    return m(
        '.details-panel',
        m('.details-panel-heading.aggregation',
          attrs.data.extra !== undefined &&
                  attrs.data.extra.kind === 'THREAD_STATE' ?
              this.showStateSummary(attrs.data.extra) :
              null,
          this.showTimeRange(),
          m('table',
            m('tr',
              attrs.data.columns.map(
                  (col) => this.formatColumnHeading(col, attrs.kind))),
            m('tr.sum', attrs.data.columnSums.map((sum) => {
              const sumClass = sum === '' ? 'td' : 'td.sum-data';
              return m(sumClass, sum);
            })))),
        m(
            '.details-table.aggregation',
            m('table', this.getRows(attrs.data)),
            ));
  }

  formatColumnHeading(col: Column, id: string) {
    const pref = globals.state.aggregatePreferences[id];
    let sortIcon = '';
    if (pref && pref.sorting && pref.sorting.column === col.columnId) {
      sortIcon = pref.sorting.direction === 'DESC' ? 'arrow_drop_down' :
                                                     'arrow_drop_up';
    }
    return m(
        'th',
        {
          onclick: () => {
            globals.dispatch(
                Actions.updateAggregateSorting({id, column: col.columnId}));
          },
        },
        col.title,
        m('i.material-icons', sortIcon));
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
    const selection = globals.state.currentSelection;
    if (selection === null || selection.kind !== 'AREA') return undefined;
    const selectedArea = globals.state.areas[selection.areaId];
    const rangeDurationMs = (selectedArea.endSec - selectedArea.startSec) * 1e3;
    return m('.time-range', `Selected range: ${rangeDurationMs.toFixed(6)} ms`);
  }

  // Thread state aggregation panel only
  showStateSummary(data: ThreadStateExtra) {
    if (data === undefined) return undefined;
    const states = [];
    for (let i = 0; i < data.states.length; i++) {
      const color = colorForState(data.states[i]);
      const textColor = textColorForState(data.states[i]);
      const width = data.values[i] / data.totalMs * 100;
      states.push(
          m('.state',
            {
              style: {
                background: `hsl(${color.h},${color.s}%,${color.l}%)`,
                color: `${textColor}`,
                width: `${width}%`,
              },
            },
            `${data.states[i]}: ${data.values[i]} ms`));
    }
    return m('.states', states);
  }

  renderCanvas() {}
}
