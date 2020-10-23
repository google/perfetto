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

import {Actions} from '../common/actions';

import {globals} from './globals';
import {Panel, PanelSize} from './panel';
import {findUiTrackId} from './scroll_helper';

export class FlowEventsPanel extends Panel {
  view() {
    const selection = globals.state.currentSelection;
    if (!selection || selection.kind !== 'CHROME_SLICE') {
      return;
    }

    const flowClickHandler = (sliceId: number, trackId: number) => {
      const uiTrackId = findUiTrackId(trackId);
      if (uiTrackId) {
        globals.makeSelection(
            Actions.selectChromeSlice(
                {id: sliceId, trackId: uiTrackId, table: 'slice'}),
            'bound_flows');
      }
    };

    // Can happen only for flow events version 1
    const haveCategories =
        globals.connectedFlows.filter(flow => flow.category).length > 0;

    const columns = [
      m('th', 'Direction'),
      m('th', 'Connected Slice ID'),
      m('th', 'Connected Slice Name')
    ];

    if (haveCategories) {
      columns.push(m('th', 'Flow Category'));
      columns.push(m('th', 'Flow Name'));
    }

    const rows = [m('tr', columns)];

    // Fill the table with all the directly connected flow events
    globals.connectedFlows.forEach(flow => {
      if (selection.id !== flow.begin.sliceId &&
          selection.id !== flow.end.sliceId) {
        return;
      }

      const outgoing = selection.id === flow.begin.sliceId;
      const otherEnd = (outgoing ? flow.end : flow.begin);

      const args = {
        onclick: () => flowClickHandler(otherEnd.sliceId, otherEnd.trackId),
        onmousemove: () =>
            globals.frontendLocalState.setHighlightedSliceId(otherEnd.sliceId),
        onmouseleave: () => globals.frontendLocalState.setHighlightedSliceId(-1)
      };

      const data = [
        m('td.flow-link', args, outgoing ? 'Outgoing' : 'Incoming'),
        m('td.flow-link', args, otherEnd.sliceId.toString()),
        m('td.flow-link', args, otherEnd.sliceName)
      ];

      if (haveCategories) {
        data.push(m('td.flow-info', flow.category || '-'));
        data.push(m('td.flow-info', flow.name || '-'));
      }

      rows.push(m('tr', data));
    });

    return m('.details-panel', [
      m('.details-panel-heading', m('h2', `Flow events`)),
      m('.flow-events-table', m('table.half-width', rows))
    ]);
  }

  renderCanvas(_ctx: CanvasRenderingContext2D, _size: PanelSize) {}
}
