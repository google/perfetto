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
import {Grid, GridHeaderCell, GridCell} from '../../widgets/grid';
import {Checkbox} from '../../widgets/checkbox';
import {Intent} from '../../widgets/common';
import {Icon} from '../../widgets/icon';
import {Tooltip} from '../../widgets/tooltip';
import {ALL_CATEGORIES, getFlowCategories} from '../../core/flow_types';
import {TraceImpl} from '../../core/trace_impl';

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

    interface FlowRow {
      category: string;
      count: number;
      isAll: number;
    }

    const rows: FlowRow[] = [];

    // 'All' row
    rows.push({
      category: 'All',
      count: flows.selectedFlows.length,
      isAll: 1,
    });

    categoryToFlowsNum.forEach((num, cat) => {
      rows.push({
        category: cat,
        count: num,
        isAll: 0,
      });
    });

    return m(Grid, {
      columns: [
        {
          key: 'category',
          header: m(GridHeaderCell, 'Flow Category'),
        },
        {
          key: 'count',
          header: m(GridHeaderCell, 'Number of flows'),
        },
        {
          key: 'show',
          header: m(
            GridHeaderCell,
            m(
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
          ),
        },
      ],
      rowData: rows.map((row) => {
        const wasChecked =
          flows.visibleCategories.get(row.category) ||
          flows.visibleCategories.get(ALL_CATEGORIES);

        return [
          m(GridCell, row.category),
          m(GridCell, {align: 'right'}, row.count),
          m(
            GridCell,
            m(Checkbox, {
              checked: wasChecked,
              onclick: () => {
                if (row.isAll === 1) {
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
                  flows.setCategoryVisible(row.category, !wasChecked);
                }
              },
            }),
          ),
        ];
      }),
      fillHeight: true,
    });
  }
}
