// Copyright (C) 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
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
import {isStackPivot} from '../common/pivot_table_common';

import {globals} from './globals';
import {hideModel} from './modal';
import {
  PivotTableHelper,
} from './pivot_table_helper';

interface PivotTableEditorAttrs {
  helper: PivotTableHelper;
}

export class ColumnPicker implements m.ClassComponent<PivotTableEditorAttrs> {
  view(vnode: m.Vnode<PivotTableEditorAttrs>) {
    const {helper} = vnode.attrs;

    // Fills available aggregations options in aggregation select.
    const aggregationOptions = [];
    for (const aggregation of helper.availableAggregations) {
      aggregationOptions.push(
          m('option', {value: aggregation, key: aggregation}, aggregation));
    }

    // Fills available columns options divided according to their table in
    // column select.
    const columnOptionGroup = [];
    for (const {tableName, columns} of helper.availableColumns) {
      const options = [];
      for (const column of columns) {
        // We can't aggregate a stack column.
        const hidden = !helper.isPivot && isStackPivot(tableName, column);
        options.push(m('option', {value: column, key: column, hidden}, column));
      }
      columnOptionGroup.push(m('optgroup', {label: tableName}, options));
    }

    return m(
        'div',
        m(
            'section',
            m('h2', 'Select column type: '),
            // Pivot radio button.
            m(
                'span',
                m(`input[type=radio][name=type][id=pivot]`, {
                  checked: helper.isPivot,
                  onchange: () => {
                    helper.togglePivotSelection();
                    globals.rafScheduler.scheduleFullRedraw();
                  }
                }),
                m(`label[for=pivot]`, 'Pivot'),
                ),
            // Aggregation radio button.
            m('span', m(`input[type=radio][name=type][id=aggregation]`, {
                checked: !helper.isPivot,
                onchange: () => {
                  helper.togglePivotSelection();
                  globals.rafScheduler.scheduleFullRedraw();
                }
              })),
            m(`label[for=aggregation]`, 'Aggregation'),
            ),
        m(
            'section',
            m('h2', 'Select a column: '),
            // Aggregation select.
            m('select',
              {
                disabled: helper.isPivot,
                selectedIndex: helper.selectedAggregationIndex,
                onchange: (e: InputEvent) => {
                  helper.setSelectedPivotTableAggregationIndex(
                      (e.target as HTMLSelectElement).selectedIndex);
                }
              },
              aggregationOptions),
            ' ',
            // Column select.
            m('select',
              {
                selectedIndex: helper.selectedColumnIndex,
                onchange: (e: InputEvent) => {
                  helper.setSelectedPivotTableColumnIndex(
                      (e.target as HTMLSelectElement).selectedIndex);
                }
              },
              columnOptionGroup),
            ),
        m('section.button-group',
          // Button to toggle selected column.
          m('button',
            {
              onclick: () => {
                helper.updatePivotTableColumnOnSelectedIndex();
                globals.rafScheduler.scheduleFullRedraw();
              }
            },
            'Add/Remove'),
          // Button to clear table and all selected columns.
          m('button',
            {
              onclick: () => {
                helper.clearPivotTableColumns();
                globals.rafScheduler.scheduleFullRedraw();
              }
            },
            'Clear')));
  }
}

export class ColumnDisplay implements m.ClassComponent<PivotTableEditorAttrs> {
  view(vnode: m.Vnode<PivotTableEditorAttrs>) {
    const {helper} = vnode.attrs;
    const selectedPivotsDisplay = [];
    const selectedAggregationsDisplay = [];

    for (let i = 0; i < helper.selectedPivots.length; ++i) {
      const columnAttrs = helper.selectedPivots[i];
      selectedPivotsDisplay.push(m(
          'tr',
          m('td',
            {
              draggable: true,
              ondragstart: (e: DragEvent) => {
                helper.selectedColumnOnDrag(e, true, i);
              },
              ondrop: (e: DragEvent) => {
                helper.removeHighlightFromDropLocation(e);
                helper.selectedColumnOnDrop(e, true, i);
                globals.rafScheduler.scheduleFullRedraw();
              },
              onclick: () => {
                helper.selectPivotTableColumn(columnAttrs);
                globals.rafScheduler.scheduleFullRedraw();
              },
              ondragenter: (e: DragEvent) => {
                helper.highlightDropLocation(e, true);
              },
              ondragleave: (e: DragEvent) => {
                helper.removeHighlightFromDropLocation(e);
              }
            },
            m('i.material-icons',
              {
                onclick: () => {
                  helper.updatePivotTableColumnOnColumnAttributes(columnAttrs);
                  globals.rafScheduler.scheduleFullRedraw();
                },
              },
              'remove'),
            ' ',
            `${columnAttrs.tableName} ${columnAttrs.columnName}`)));
    }

    for (let i = 0; i < helper.selectedAggregations.length; ++i) {
      const columnAttrs = helper.selectedAggregations[i];
      const sortIcon = helper.selectedAggregations[i].order === 'DESC' ?
          'arrow_drop_down' :
          'arrow_drop_up';
      selectedAggregationsDisplay.push(m(
          'tr',
          m('td',
            {
              draggable: 'true',
              ondragstart: (e: DragEvent) => {
                helper.selectedColumnOnDrag(e, false, i);
              },
              ondrop: (e: DragEvent) => {
                helper.removeHighlightFromDropLocation(e);
                helper.selectedColumnOnDrop(e, false, i);
                globals.rafScheduler.scheduleFullRedraw();
              },
              onclick: () => {
                helper.selectPivotTableColumn(columnAttrs);
                globals.rafScheduler.scheduleFullRedraw();
              },
              ondragenter: (e: DragEvent) => {
                helper.highlightDropLocation(e, false);
              },
              ondragleave: (e: DragEvent) => {
                helper.removeHighlightFromDropLocation(e);
              }
            },
            m('i.material-icons',
              {
                onclick: () => {
                  helper.updatePivotTableColumnOnColumnAttributes(columnAttrs);
                  globals.rafScheduler.scheduleFullRedraw();
                },
              },
              'remove'),
            ' ',
            `${columnAttrs.tableName} ${columnAttrs.columnName} (${
                columnAttrs.aggregation})`,
            m('i.material-icons',
              {
                onclick: () => {
                  helper.togglePivotTableAggregationSorting(i);
                  globals.rafScheduler.scheduleFullRedraw();
                }
              },
              sortIcon))));
    }

    return m(
        'div',
        m('section.table-group',
          // Table that displays selected pivots.
          m('table',
            m('thead', m('tr', m('th', 'Selected Pivots'))),
            m('div.scroll', m('tbody', selectedPivotsDisplay))),
          // Table that displays selected aggregations.
          m('table',
            m('thead', m('tr', m('th', 'Selected Aggregations'))),
            m('div.scroll', m('tbody', selectedAggregationsDisplay)))),
        m('section.button-group',
          // Button to toggle selected column.
          m('button',
            {
              onclick: () => {
                helper.queryPivotTableChanges();
                hideModel();
              }
            },
            'Query'),
          // Button to clear table and all selected columns.
          m('button',
            {
              onclick: () => {
                hideModel();
              }
            },
            'Cancel')));
  }
}
