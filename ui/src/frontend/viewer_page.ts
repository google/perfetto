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

/**
 * Top-most level component for the viewer page. Holds tracks, brush timeline,
 * panels, and everything else that's part of the main trace viewer page.
 */
const TraceViewer = {
  oninit() {
    this.width = 0;
    this.maxVisibleWindowMs = {start: 0, end: 10000000};
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
          m('header.tracks-content', 'Tracks'),
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
}>;

export const ViewerPage = createPage({
  view() {
    return m(TraceViewer);
  }
});
