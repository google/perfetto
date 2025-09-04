// Copyright (C) 2025 The Android Open Source Project
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

import m from 'mithril';
import {
  ColumnDefinition,
  RowDef,
} from '../../components/widgets/data_grid/common';
import {DataGrid} from '../../components/widgets/data_grid/data_grid';
import {Checkbox} from '../../widgets/checkbox';
import {Intent} from '../../widgets/common';
import {Icon} from '../../widgets/icon';
import {Tooltip} from '../../widgets/tooltip';
import {ALL_CATEGORIES, getFlowCategories} from '../../core/flow_types';
import {TraceImpl} from '../../core/trace_impl';
import {SqlValue} from '../../trace_processor/query_result';

export interface FlowEventsAreaSelectedPanelAttrs {
  trace: TraceImpl;
}

export class FlowEventsAreaSelectedPanel
  implements m.ClassComponent<FlowEventsAreaSelectedPanelAttrs>
{
  view({attrs}: m.CVnode<FlowEventsAreaSelectedPanelAttrs>) {
    const selection = attrs.trace.selection.selection;
    if (selection.kind !== 'area') {
      return;
    }

    const {trace} = attrs;
    const {flows} = trace;

    const categoryToFlowsNum = new Map<string, number>();

    flows.selectedFlows.forEach((flow) => {
      const categories = getFlowCategories(flow);
      categories.forEach((cat) => {
        if (!categoryToFlowsNum.has(cat)) {
          categoryToFlowsNum.set(cat, 0);
        }
        categoryToFlowsNum.set(cat, categoryToFlowsNum.get(cat)! + 1);
      });
    });

    const rows: RowDef[] = [];

    // 'All' row
    rows.push({
      category: 'All',
      count: flows.selectedFlows.length,
      show: 'checkbox', // value is not used, just need to trigger the renderer
      isAll: 1,
    });

    categoryToFlowsNum.forEach((num, cat) => {
      rows.push({
        category: cat,
        count: num,
        show: 'checkbox',
        isAll: 0,
      });
    });

    const columns: ColumnDefinition[] = [
      {
        name: 'category',
        title: 'Flow Category',
      },
      {
        name: 'count',
        title: 'Number of flows',
      },
      {
        name: 'show',
        title: m(
          'span',
          'Show ',
          m(
            Tooltip,
            {
              trigger: m(Icon, {icon: 'warning', intent: Intent.Warning}),
            },
            'Showing a large number of flows may impact performance.',
          ),
        ),
      },
    ];

    function cellRenderer(value: SqlValue, colName: string, row: RowDef) {
      const {isAll, category} = row;

      if (colName === 'show') {
        const wasChecked =
          flows.visibleCategories.get(category as string) ||
          flows.visibleCategories.get(ALL_CATEGORIES);

        return m(Checkbox, {
          checked: wasChecked,
          onclick: () => {
            if (isAll === 1) {
              if (wasChecked) {
                for (const k of flows.visibleCategories.keys()) {
                  flows.setCategoryVisible(k, false);
                }
              } else {
                categoryToFlowsNum.forEach((_, cat) => {
                  flows.setCategoryVisible(cat, true);
                });
              }
              flows.setCategoryVisible(ALL_CATEGORIES, !wasChecked);
            } else {
              if (wasChecked) {
                flows.setCategoryVisible(ALL_CATEGORIES, false);
              }
              flows.setCategoryVisible(category as string, !wasChecked);
            }
          },
        });
      }

      if (value === null) {
        return m(`span`, 'null');
      }
      return m(`span`, `${value}`);
    }

    return m(DataGrid, {
      columns,
      data: rows,
      cellRenderer,
      // Readonly filters and sorting - don't allow the user to change these.
      sorting: {
        direction: 'UNSORTED',
      },
      filters: [],
      fillHeight: true,
    });
  }
}
