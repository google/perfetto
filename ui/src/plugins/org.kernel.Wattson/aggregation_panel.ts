// Copyright (C) 2025 The Android Open Source Project
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
import {createBaseAggregationToTabAdaptor} from '../../components/aggregation_adapter';
import {
  AggState,
  AggregationPanel,
  AggregationPanelAttrs,
} from '../../components/aggregation_panel';
import {translateState} from '../../components/sql_utils/thread_state';
import {AggregateData, Column} from '../../public/aggregation';
import {
  AreaSelectionAggregator,
  AreaSelectionTab,
} from '../../public/selection';
import {Trace} from '../../public/trace';
import {SegmentedButtons} from '../../widgets/segmented_buttons';

export function createWattsonAggregationToTabAdaptor(
  trace: Trace,
  aggregator: AreaSelectionAggregator,
  tabPriorityOverride?: number,
): AreaSelectionTab {
  return createBaseAggregationToTabAdaptor(
    trace,
    aggregator,
    WattsonAggregationPanel,
    tabPriorityOverride,
  );
}

export class WattsonAggregationPanel
  extends AggregationPanel
  implements m.ClassComponent<AggregationPanelAttrs>
{
  private scaleNumericData: boolean = false;

  view({attrs}: m.CVnode<AggregationPanelAttrs>) {
    return m(
      '.details-panel',
      m(
        '.details-panel-heading.aggregation',
        m(SegmentedButtons, {
          options: [{label: 'µW'}, {label: 'mW'}],
          selectedOption: this.scaleNumericData ? 0 : 1,
          onOptionSelected: (index) => {
            this.scaleNumericData = index === 0;
          },
          title: 'Select power units',
        }),
        attrs.data.extra !== undefined &&
          attrs.data.extra.kind === 'THREAD_STATE'
          ? this.showStateSummary(attrs.data.extra)
          : null,
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
              let sumClass: string;
              let displaySum: string;

              if (sum === '') {
                sumClass = 'td';
                displaySum = String(sum);
              } else {
                sumClass = 'td.sum-data';
                displaySum = String(
                  this.scaleNumericData ? parseFloat(sum) * 1000 : sum,
                );
              }

              return m(sumClass, displaySum);
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

    // Replace title units if necessary (i.e. swap µW/mW)
    let displayTitle = col.title;
    if (this.scaleNumericData) {
      displayTitle = displayTitle.replace('estimated mW', 'estimated µW');
    }
    return m(
      'th',
      {
        onclick: () => {
          model.toggleSortingColumn(col.columnId);
        },
      },
      displayTitle,
      m('i.material-icons', sortIcon),
    );
  }

  getFormattedData(data: AggregateData, rowIndex: number, columnIndex: number) {
    const column = data.columns[columnIndex];
    const value = column.data[rowIndex];

    switch (column.kind) {
      case 'STRING':
        return data.strings?.[value as number] ?? value;
      case 'TIMESTAMP_NS':
        return `${Number(value) / 1000000}`;
      case 'STATE': {
        const concatState = data.strings?.[value as number];
        if (typeof concatState !== 'string') return value;
        const split = concatState.split(',');
        const ioWait =
          split[1] === 'NULL' ? undefined : !!Number.parseInt(split[1], 10);
        return translateState(split[0], ioWait);
      }
      case 'NUMBER':
      default:
        if (typeof value === 'number') {
          return this.scaleNumericData && column.title.includes('estimated mW')
            ? value * 1000
            : value;
        }
        return value;
    }
  }
}
