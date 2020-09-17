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
    const flowsIn =
        globals.boundFlows.filter(flow => flow.end.sliceId === selection.id);
    const flowsOut =
        globals.boundFlows.filter(flow => flow.begin.sliceId === selection.id);

    const flowClickHandler = (sliceId: number, trackId: number) => {
      const uiTrackId = findUiTrackId(trackId);
      if (uiTrackId) {
        globals.makeSelection(
            Actions.selectChromeSlice(
                {id: sliceId, trackId: uiTrackId, table: 'slice'}),
            'bound_flows');
      }
    };
    const incomingFlowsTable: m.Vnode[] = [
      m('.details-panel-heading', m('h2', `Incoming flow events`)),
      m(
          '.flow-events-table',
          m('table.half-width',
            m('tr', m('th', 'Source Slice ID'), m('th', 'Source Slice Name')),
            flowsIn.map(flow => {
              const args = {
                onclick: () =>
                    flowClickHandler(flow.begin.sliceId, flow.begin.trackId),
                onmousemove: () =>
                    globals.frontendLocalState.setHighlightedSliceId(
                        flow.begin.sliceId),
                onmouseleave: () =>
                    globals.frontendLocalState.setHighlightedSliceId(-1)
              };
              return m(
                  'tr',
                  m('td.flow-link', args, flow.begin.sliceId.toString()),
                  m('td.flow-link', args, flow.begin.sliceName));
            })),
          )
    ];
    const outgoingFlowsTable: m.Vnode[] = [
      m('.details-panel-heading', m('h2', `Outgoing flow events`)),
      m(
          '.flow-events-table',
          m('table.half-width',
            m('tr',
              m('th', 'Destination Slice ID'),
              m('th', 'Destination Slice Name')),
            flowsOut.map(flow => {
              const args = {
                onclick: () =>
                    flowClickHandler(flow.end.sliceId, flow.end.trackId),
                onmousemove: () =>
                    globals.frontendLocalState.setHighlightedSliceId(
                        flow.end.sliceId),
                onmouseleave: () =>
                    globals.frontendLocalState.setHighlightedSliceId(-1)
              };
              return m(
                  'tr',
                  m('td.flow-link', args, flow.end.sliceId.toString()),
                  m('td.flow-link', args, flow.end.sliceName));
            })),
          )
    ];
    return m(
        '.details-panel',
        flowsIn.length > 0 ? incomingFlowsTable : [],
        flowsOut.length > 0 ? outgoingFlowsTable : []);
  }

  renderCanvas(_ctx: CanvasRenderingContext2D, _size: PanelSize) {}
}
