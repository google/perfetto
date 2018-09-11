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

import {FlameGraphPanel} from './flame_graph_panel';
import {globals} from './globals';
import {HeaderPanel} from './header_panel';
import {OverviewTimelinePanel} from './overview_timeline_panel';
import {createPage} from './pages';
import {PanAndZoomHandler} from './pan_and_zoom_handler';
import {PanelContainer} from './panel_container';
import {TimeAxisPanel} from './time_axis_panel';
import {TRACK_SHELL_WIDTH} from './track_panel';
import {TrackPanel} from './track_panel';

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
    const updateDimensions = () => {
      const rect = vnode.dom.getBoundingClientRect();
      this.width = rect.width;
      frontendLocalState.timeScale.setLimitsPx(
          0, this.width - TRACK_SHELL_WIDTH);
    };

    updateDimensions();

    // TODO: Do resize handling better.
    this.onResize = () => {
      updateDimensions();
      globals.rafScheduler.scheduleFullRedraw();
    };

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
    const scrollingPanels = globals.state.displayedTrackIds.length > 0 ?
        [
          m(HeaderPanel, {title: 'Tracks'}),
          ...globals.state.displayedTrackIds.map(id => m(TrackPanel, {id})),
          m(FlameGraphPanel),
        ] :
        [];
    return m(
        '.page',
        m(QueryTable),
        // TODO: Pan and zoom logic should be in its own mithril component.
        m('.pan-and-zoom-content',
          m('.pinned-panel-container', m(PanelContainer, {
              doesScroll: false,
              panels: [
                m(OverviewTimelinePanel),
                m(TimeAxisPanel),
              ],
            })),
          m('.scrolling-panel-container', m(PanelContainer, {
              doesScroll: true,
              panels: scrollingPanels,
            }))));
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
