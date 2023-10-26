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
import {Actions} from '../common/actions';
import {raf} from '../core/raf_scheduler';
import {DurationWidget} from '../widgets/duration';

import {Flow, globals} from './globals';

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

export class FlowEventsPanel implements m.ClassComponent {
  view() {
    const selection = globals.state.currentSelection;
    if (!selection || selection.kind !== 'CHROME_SLICE') {
      return;
    }

    const flowClickHandler = (sliceId: number, trackId: number) => {
      const trackKey = globals.state.trackKeyByTrackId[trackId];
      if (trackKey) {
        globals.makeSelection(
            Actions.selectChromeSlice({id: sliceId, trackKey, table: 'slice'}),
            {tab: 'bound_flows'});
      }
    };

    // Can happen only for flow events version 1
    const haveCategories =
        globals.connectedFlows.filter((flow) => flow.category).length > 0;

    const columns = [
      m('th', 'Direction'),
      m('th', 'Duration'),
      m('th', 'Connected Slice ID'),
      m('th', 'Connected Slice Name'),
      m('th', 'Thread Out'),
      m('th', 'Thread In'),
      m('th', 'Process Out'),
      m('th', 'Process In'),
    ];

    if (haveCategories) {
      columns.push(m('th', 'Flow Category'));
      columns.push(m('th', 'Flow Name'));
    }

    const rows = [m('tr', columns)];

    // Fill the table with all the directly connected flow events
    globals.connectedFlows.forEach((flow) => {
      if (selection.id !== flow.begin.sliceId &&
          selection.id !== flow.end.sliceId) {
        return;
      }

      const outgoing = selection.id === flow.begin.sliceId;
      const otherEnd = (outgoing ? flow.end : flow.begin);

      const args = {
        onclick: () => flowClickHandler(otherEnd.sliceId, otherEnd.trackId),
        onmousemove: () => globals.dispatch(
            Actions.setHighlightedSliceId({sliceId: otherEnd.sliceId})),
        onmouseleave: () =>
            globals.dispatch(Actions.setHighlightedSliceId({sliceId: -1})),
      };

      const data = [
        m('td.flow-link', args, outgoing ? 'Outgoing' : 'Incoming'),
        m('td.flow-link', args, m(DurationWidget, {dur: flow.dur})),
        m('td.flow-link', args, otherEnd.sliceId.toString()),
        m('td.flow-link', args, otherEnd.sliceName),
        m('td.flow-link', args, flow.begin.threadName),
        m('td.flow-link', args, flow.end.threadName),
        m('td.flow-link', args, flow.begin.processName),
        m('td.flow-link', args, flow.end.processName),
      ];

      if (haveCategories) {
        data.push(m('td.flow-info', flow.category || '-'));
        data.push(m('td.flow-info', flow.name || '-'));
      }

      rows.push(m('tr', data));
    });

    return m('.details-panel', [
      m('.details-panel-heading', m('h2', `Flow events`)),
      m('.flow-events-table', m('table', rows)),
    ]);
  }
}

export class FlowEventsAreaSelectedPanel implements m.ClassComponent {
  view() {
    const selection = globals.state.currentSelection;
    if (!selection || selection.kind !== 'AREA') {
      return;
    }

    const columns = [
      m('th', 'Flow Category'),
      m('th', 'Number of flows'),
      m('th',
        'Show',
        m('a.warning',
          m('i.material-icons', 'warning'),
          m('.tooltip',
            'Showing a large number of flows may impact performance.'))),
    ];

    const rows = [m('tr', columns)];

    const categoryToFlowsNum = new Map<string, number>();

    globals.selectedFlows.forEach((flow) => {
      const categories = getFlowCategories(flow);
      categories.forEach((cat) => {
        if (!categoryToFlowsNum.has(cat)) {
          categoryToFlowsNum.set(cat, 0);
        }
        categoryToFlowsNum.set(cat, categoryToFlowsNum.get(cat)! + 1);
      });
    });

    const allWasChecked = globals.visibleFlowCategories.get(ALL_CATEGORIES);
    rows.push(m('tr.sum', [
      m('td.sum-data', 'All'),
      m('td.sum-data', globals.selectedFlows.length),
      m('td.sum-data',
        m('i.material-icons',
          {
            onclick: () => {
              if (allWasChecked) {
                globals.visibleFlowCategories.clear();
              } else {
                categoryToFlowsNum.forEach((_, cat) => {
                  globals.visibleFlowCategories.set(cat, true);
                });
              }
              globals.visibleFlowCategories.set(ALL_CATEGORIES, !allWasChecked);
              raf.scheduleFullRedraw();
            },
          },
          allWasChecked ? Icons.Checkbox : Icons.BlankCheckbox)),
    ]));

    categoryToFlowsNum.forEach((num, cat) => {
      const wasChecked = globals.visibleFlowCategories.get(cat) ||
          globals.visibleFlowCategories.get(ALL_CATEGORIES);
      const data = [
        m('td.flow-info', cat),
        m('td.flow-info', num),
        m('td.flow-info',
          m('i.material-icons',
            {
              onclick: () => {
                if (wasChecked) {
                  globals.visibleFlowCategories.set(ALL_CATEGORIES, false);
                }
                globals.visibleFlowCategories.set(cat, !wasChecked);
                raf.scheduleFullRedraw();
              },
            },
            wasChecked ? Icons.Checkbox : Icons.BlankCheckbox)),
      ];
      rows.push(m('tr', data));
    });

    return m('.details-panel', [
      m('.details-panel-heading', m('h2', `Selected flow events`)),
      m('.flow-events-table', m('table', rows)),
    ]);
  }
}
