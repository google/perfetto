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

import m from 'mithril';
import {Icons} from '../base/semantic_icons';
import {raf} from '../core/raf_scheduler';
import {Flow} from '../core/flow_types';
import {TraceImpl} from '../core/trace_impl';

export const ALL_CATEGORIES = '_all_';

export function getFlowCategories(flow: Flow): string[] {
  const categories: string[] = [];
  // v1 flows have their own categories
  if (flow.category) {
    categories.push(...flow.category.split(','));
    return categories;
  }
  const beginCats = flow.begin.sliceCategory.split(',');
  const endCats = flow.end.sliceCategory.split(',');
  categories.push(...new Set([...beginCats, ...endCats]));
  return categories;
}

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

    const columns = [
      m('th', 'Flow Category'),
      m('th', 'Number of flows'),
      m(
        'th',
        'Show',
        m(
          'a.warning',
          m('i.material-icons', 'warning'),
          m(
            '.tooltip',
            'Showing a large number of flows may impact performance.',
          ),
        ),
      ),
    ];

    const rows = [m('tr', columns)];

    const categoryToFlowsNum = new Map<string, number>();

    const flows = attrs.trace.flows;
    flows.selectedFlows.forEach((flow) => {
      const categories = getFlowCategories(flow);
      categories.forEach((cat) => {
        if (!categoryToFlowsNum.has(cat)) {
          categoryToFlowsNum.set(cat, 0);
        }
        categoryToFlowsNum.set(cat, categoryToFlowsNum.get(cat)! + 1);
      });
    });

    const allWasChecked = flows.visibleCategories.get(ALL_CATEGORIES);
    rows.push(
      m('tr.sum', [
        m('td.sum-data', 'All'),
        m('td.sum-data', flows.selectedFlows.length),
        m(
          'td.sum-data',
          m(
            'i.material-icons',
            {
              onclick: () => {
                if (allWasChecked) {
                  for (const k of flows.visibleCategories.keys()) {
                    flows.setCategoryVisible(k, false);
                  }
                } else {
                  categoryToFlowsNum.forEach((_, cat) => {
                    flows.setCategoryVisible(cat, true);
                  });
                }
                flows.setCategoryVisible(ALL_CATEGORIES, !allWasChecked);
              },
            },
            allWasChecked ? Icons.Checkbox : Icons.BlankCheckbox,
          ),
        ),
      ]),
    );

    categoryToFlowsNum.forEach((num, cat) => {
      const wasChecked =
        flows.visibleCategories.get(cat) ||
        flows.visibleCategories.get(ALL_CATEGORIES);
      const data = [
        m('td.flow-info', cat),
        m('td.flow-info', num),
        m(
          'td.flow-info',
          m(
            'i.material-icons',
            {
              onclick: () => {
                if (wasChecked) {
                  flows.setCategoryVisible(ALL_CATEGORIES, false);
                }
                flows.setCategoryVisible(cat, !wasChecked);
                raf.scheduleFullRedraw();
              },
            },
            wasChecked ? Icons.Checkbox : Icons.BlankCheckbox,
          ),
        ),
      ];
      rows.push(m('tr', data));
    });

    return m('.details-panel', [
      m('.details-panel-heading', m('h2', `Selected flow events`)),
      m('.flow-events-table', m('table', rows)),
    ]);
  }
}
