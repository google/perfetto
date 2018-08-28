// Copyright (C) 2018 The Android Open Source Project
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

import {QueryResponse} from '../common/queries';
import {TimeSpan} from '../common/time';

import {globals} from './globals';
import {createPage} from './pages';
import {PanAndZoomHandler} from './pan_and_zoom_handler';
import {ScrollingPanelContainer} from './scrolling_panel_container';
import {TopPanelContainer} from './top_panel_container';
import {TRACK_SHELL_WIDTH} from './track_panel';

const MAX_ZOOM_SPAN_SEC = 1e-4;  // 0.1 ms.

const QueryTable: m.Component<{}, {}> = {
  view() {
    const resp = globals.queryResults.get('command') as QueryResponse;
    if (resp === undefined) {
      return m('');
    }
    const cols = [];
    for (const col of resp.columns) {
      cols.push(m('td', col));
    }
    const header = m('tr', cols);

    const rows = [];
    for (let i = 0; i < resp.rows.length; i++) {
      const cells = [];
      for (const col of resp.columns) {
        cells.push(m('td', resp.rows[i][col]));
      }
      rows.push(m('tr', cells));
    }
    return m(
        'div',
        m('header.overview',
          `Query result - ${Math.round(resp.durationMs)} ms`,
          m('span.code', resp.query)),
        resp.error ?
            m('.query-error', `SQL error: ${resp.error}`) :
            m('table.query-table', m('thead', header), m('tbody', rows)));
  },
};

/**
 * Top-most level component for the viewer page. Holds tracks, brush timeline,
 * panels, and everything else that's part of the main trace viewer page.
 */
const TraceViewer = {
  oninit() {
    this.width = 0;
  },

  oncreate(vnode) {
    const frontendLocalState = globals.frontendLocalState;
    // TODO: This is probably not needed. Figure out resizing.
    this.onResize = () => {
      const rect = vnode.dom.getBoundingClientRect();
      this.width = rect.width;
      frontendLocalState.timeScale.setLimitsPx(
          0, this.width - TRACK_SHELL_WIDTH);
    };

    // Have to redraw after initialization to provide dimensions to view().
    setTimeout(() => this.onResize());

    // Once ResizeObservers are out, we can stop accessing the window here.
    window.addEventListener('resize', this.onResize);

    const panZoomEl =
        vnode.dom.querySelector('.pan-and-zoom-content') as HTMLElement;

    this.zoomContent = new PanAndZoomHandler({
      element: panZoomEl,
      contentOffsetX: TRACK_SHELL_WIDTH,
      onPanned: (pannedPx: number) => {
        const traceTime = globals.state.traceTime;
        const vizTime = globals.frontendLocalState.visibleWindowTime;
        const origDelta = vizTime.duration;
        const tDelta = frontendLocalState.timeScale.deltaPxToDuration(pannedPx);
        let tStart = vizTime.start + tDelta;
        let tEnd = vizTime.end + tDelta;
        if (tStart < traceTime.startSec) {
          tStart = traceTime.startSec;
          tEnd = tStart + origDelta;
        } else if (tEnd > traceTime.endSec) {
          tEnd = traceTime.endSec;
          tStart = tEnd - origDelta;
        }
        frontendLocalState.updateVisibleTime(new TimeSpan(tStart, tEnd));
      },
      onZoomed: (_: number, zoomRatio: number) => {
        const vizTime = frontendLocalState.visibleWindowTime;
        const curSpanSec = vizTime.duration;
        const newSpanSec =
            Math.max(curSpanSec - curSpanSec * zoomRatio, MAX_ZOOM_SPAN_SEC);
        const deltaSec = (curSpanSec - newSpanSec) / 2;
        const newStartSec = vizTime.start + deltaSec;
        const newEndSec = vizTime.end - deltaSec;
        frontendLocalState.updateVisibleTime(
            new TimeSpan(newStartSec, newEndSec));
      }
    });
  },

  onremove() {
    window.removeEventListener('resize', this.onResize);
    this.zoomContent.shutdown();
  },

  view() {
    return m(
        '.page',
        m(QueryTable),
        // TODO: Pan and zoom logic should be in its own mithril component.
        m('.pan-and-zoom-content',
          m(TopPanelContainer),
          m(ScrollingPanelContainer)));
  },

} as m.Component<{}, {
  onResize: () => void,
  width: number,
  zoomContent: PanAndZoomHandler,
  overviewQueryExecuted: boolean,
  overviewQueryResponse: QueryResponse,
}>;

export const ViewerPage = createPage({
  view() {
    return m(TraceViewer);
  }
});
