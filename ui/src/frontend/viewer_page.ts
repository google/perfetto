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

import {executeQuery} from '../common/actions';
import {QueryResponse} from '../common/queries';
import {EngineConfig} from '../common/state';

import {globals} from './globals';
import {OverviewTimeline} from './overview_timeline';
import {createPage} from './pages';
import {PanAndZoomHandler} from './pan_and_zoom_handler';
import {rafScheduler} from './raf_scheduler';
import {ScrollingPanelContainer} from './scrolling_panel_container';
import {TimeAxis} from './time_axis';
import {TRACK_SHELL_WIDTH} from './track_panel';


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
          `Query result - ${resp.durationMs} ms`,
          m('span.code', resp.query)),
        resp.error ?
            m('.query-error', `SQL error: ${resp.error}`) :
            m('table.query-table', m('thead', header), m('tbody', rows)));
  },
};

export const OVERVIEW_QUERY_ID = 'overview_query';

/**
 * Top-most level component for the viewer page. Holds tracks, brush timeline,
 * panels, and everything else that's part of the main trace viewer page.
 */
const TraceViewer = {
  oninit() {
    this.width = 0;
    this.maxVisibleWindowMs = {start: 0, end: 10000000};
    this.overviewQueryExecuted = false;
  },
  oncreate(vnode) {
    const frontendLocalState = globals.frontendLocalState;
    this.onResize = () => {
      const rect = vnode.dom.getBoundingClientRect();
      this.width = rect.width;
      frontendLocalState.timeScale.setLimitsPx(
          0, this.width - TRACK_SHELL_WIDTH);
      m.redraw();
    };

    // Have to redraw after initialization to provide dimensions to view().
    setTimeout(() => this.onResize());

    // Once ResizeObservers are out, we can stop accessing the window here.
    window.addEventListener('resize', this.onResize);

    const panZoomEl =
        vnode.dom.getElementsByClassName('tracks-content')[0] as HTMLElement;

    this.zoomContent = new PanAndZoomHandler({
      element: panZoomEl,
      contentOffsetX: TRACK_SHELL_WIDTH,
      onPanned: (pannedPx: number) => {
        const deltaMs =
            frontendLocalState.timeScale.deltaPxToDurationMs(pannedPx);
        const visibleWindowMs = globals.frontendLocalState.visibleWindowMs;
        visibleWindowMs.start += deltaMs;
        visibleWindowMs.end += deltaMs;
        frontendLocalState.timeScale.setLimitsMs(
            visibleWindowMs.start, visibleWindowMs.end);
        // TODO: Replace this with repaint canvas only instead of full redraw.
        m.redraw();
      },
      onZoomed: (zoomedPositionPx: number, zoomPercentage: number) => {
        const visibleWindowMs = frontendLocalState.visibleWindowMs;
        const totalTimespanMs = visibleWindowMs.end - visibleWindowMs.start;
        const newTotalTimespanMs = totalTimespanMs * zoomPercentage;

        const zoomedPositionMs =
            frontendLocalState.timeScale.pxToMs(zoomedPositionPx) as number;
        const positionPercentage =
            (zoomedPositionMs - visibleWindowMs.start) / totalTimespanMs;

        visibleWindowMs.start =
            zoomedPositionMs - newTotalTimespanMs * positionPercentage;
        visibleWindowMs.end =
            zoomedPositionMs + newTotalTimespanMs * (1 - positionPercentage);
        frontendLocalState.timeScale.setLimitsMs(
            visibleWindowMs.start, visibleWindowMs.end);
        // TODO: Replace this with repaint canvas only instead of full redraw.
        m.redraw();
      }
    });
  },
  onremove() {
    window.removeEventListener('resize', this.onResize);
    this.zoomContent.shutdown();
  },
  onupdate() {
    rafScheduler.syncRedraw();
  },
  view() {
    const frontendLocalState = globals.frontendLocalState;
    const {visibleWindowMs} = frontendLocalState;
    const onBrushedMs = (start: number, end: number) => {
      visibleWindowMs.start = start;
      visibleWindowMs.end = end;
      globals.frontendLocalState.timeScale.setLimitsMs(
          visibleWindowMs.start, visibleWindowMs.end);
      m.redraw();
    };

    const engine: EngineConfig = globals.state.engines['0'];
    if (engine && engine.ready && !this.overviewQueryExecuted) {
      this.overviewQueryExecuted = true;
      globals.dispatch(executeQuery(
          engine.id,
          OVERVIEW_QUERY_ID,
          'select round(ts/1e9, 1) as rts, sum(dur)/1e8 as load, upid, ' +
          'process.name from slices inner join thread using(utid) inner join ' +
          'process using(upid) where depth = 0 group by rts, upid ' +
          'order by upid limit 10000'));
    }
    const resp = globals.queryResults.get(OVERVIEW_QUERY_ID) as QueryResponse;
    if (resp !== this.overviewQueryResponse) {
      this.overviewQueryResponse = resp;

      const timesMs = resp.rows.map(row => (row.rts as number) * 1000);
      const minTimeMs = Math.min(...timesMs);
      const durationMs = Math.max(...timesMs) - minTimeMs;

      const previousDurationMs =
          this.maxVisibleWindowMs.end - this.maxVisibleWindowMs.start;
      const startPercent =
          (visibleWindowMs.start - this.maxVisibleWindowMs.start) /
          previousDurationMs;
      const endPercent = (visibleWindowMs.end - this.maxVisibleWindowMs.start) /
          previousDurationMs;

      this.maxVisibleWindowMs.start = 0;
      this.maxVisibleWindowMs.end = durationMs;

      visibleWindowMs.start = durationMs * startPercent;
      visibleWindowMs.end = durationMs * endPercent;
      // TODO: Update the local and remote state.
    }

    return m(
        '.page',
        m('header.overview', 'Big picture'),
        m(OverviewTimeline, {
          // TODO: Remove global attrs.
          visibleWindowMs,
          maxVisibleWindowMs: this.maxVisibleWindowMs,
          onBrushedMs
        }),
        m(QueryTable),
        m('.tracks-content',
          {
            style: {
              width: '100%',
              // Temporary until everything is moved to panel containers.
              height: 'calc(100% - 165px)',
              position: 'relative',
            }
          },
          m('header', 'Tracks'),
          m(TimeAxis, {
            // TODO: Remove global attrs.
            timeScale: frontendLocalState.timeScale,
            contentOffset: TRACK_SHELL_WIDTH,
            visibleWindowMs,
          }),
          // Temporary hack until everything is moved to panel containers.
          m('div',
            {
              style: {
                position: 'relative',
                height: 'calc(100% - 70px)',
              }
            },
            m(ScrollingPanelContainer)), ), );
  },
} as m.Component<{}, {
  maxVisibleWindowMs: {start: number, end: number},
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
