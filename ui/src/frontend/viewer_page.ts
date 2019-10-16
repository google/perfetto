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

import {Actions} from '../common/actions';
import {QueryResponse} from '../common/queries';
import {TimeSpan} from '../common/time';

import {copyToClipboard} from './clipboard';
import {DetailsPanel} from './details_panel';
import {globals} from './globals';
import {NotesPanel} from './notes_panel';
import {OverviewTimelinePanel} from './overview_timeline_panel';
import {createPage} from './pages';
import {PanAndZoomHandler} from './pan_and_zoom_handler';
import {Panel} from './panel';
import {AnyAttrsVnode, PanelContainer} from './panel_container';
import {TickmarkPanel} from './tickmark_panel';
import {TimeAxisPanel} from './time_axis_panel';
import {computeZoom} from './time_scale';
import {TimeSelectionPanel} from './time_selection_panel';
import {TRACK_SHELL_WIDTH} from './track_constants';
import {TrackGroupPanel} from './track_group_panel';
import {TrackPanel} from './track_panel';
import {VideoPanel} from './video_panel';

const SIDEBAR_WIDTH = 256;

class QueryTable extends Panel {
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
          m('span.code', resp.query),
          resp.error ? null :
                       m('button.query-ctrl',
                         {
                           onclick: () => {
                             const lines: string[][] = [];
                             lines.push(resp.columns);
                             for (const row of resp.rows) {
                               const line = [];
                               for (const col of resp.columns) {
                                 line.push(row[col].toString());
                               }
                               lines.push(line);
                             }
                             copyToClipboard(
                                 lines.map(line => line.join('\t')).join('\n'));
                           },
                         },
                         'Copy as .tsv'),
          m('button.query-ctrl',
            {
              onclick: () => {
                globals.queryResults.delete('command');
                globals.rafScheduler.scheduleFullRedraw();
              }
            },
            'Close'), ),
        resp.error ?
            m('.query-error', `SQL error: ${resp.error}`) :
            m('table.query-table', m('thead', header), m('tbody', rows)));
  }

  renderCanvas() {}
}


// Checks if the mousePos is within 3px of the start or end of the
// current selected time range.
function onTimeRangeBoundary(mousePos: number): 'START'|'END'|null {
  const startSec = globals.frontendLocalState.selectedTimeRange.startSec;
  const endSec = globals.frontendLocalState.selectedTimeRange.endSec;
  if (startSec !== undefined && endSec !== undefined) {
    const start = globals.frontendLocalState.timeScale.timeToPx(startSec);
    const end = globals.frontendLocalState.timeScale.timeToPx(endSec);
    const startDrag = mousePos - TRACK_SHELL_WIDTH;
    const startDistance = Math.abs(start - startDrag);
    const endDistance = Math.abs(end - startDrag);
    const range = 3 * window.devicePixelRatio;
    // We might be within 3px of both boundaries but we should choose
    // the closest one.
    if (startDistance < range && startDistance <= endDistance) return 'START';
    if (endDistance < range && endDistance <= startDistance) return 'END';
  }
  return null;
}

/**
 * Top-most level component for the viewer page. Holds tracks, brush timeline,
 * panels, and everything else that's part of the main trace viewer page.
 */
class TraceViewer implements m.ClassComponent {
  private onResize: () => void = () => {};
  private zoomContent?: PanAndZoomHandler;
  // Used to prevent global deselection if a pan/drag select occurred.
  private keepCurrentSelection = false;

  oncreate(vnode: m.CVnodeDOM) {
    const frontendLocalState = globals.frontendLocalState;
    const updateDimensions = () => {
      const rect = vnode.dom.getBoundingClientRect();
      frontendLocalState.timeScale.setLimitsPx(
          0, rect.width - TRACK_SHELL_WIDTH);
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
      contentOffsetX: SIDEBAR_WIDTH,
      onPanned: (pannedPx: number) => {
        this.keepCurrentSelection = true;
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
        globals.rafScheduler.scheduleRedraw();
      },
      onZoomed: (zoomedPositionPx: number, zoomRatio: number) => {
        // TODO(hjd): Avoid hardcoding TRACK_SHELL_WIDTH.
        // TODO(hjd): Improve support for zooming in overview timeline.
        const span = frontendLocalState.visibleWindowTime;
        const scale = frontendLocalState.timeScale;
        const zoomPx = zoomedPositionPx - TRACK_SHELL_WIDTH;
        const newSpan = computeZoom(scale, span, 1 - zoomRatio, zoomPx);
        frontendLocalState.updateVisibleTime(newSpan);
        globals.rafScheduler.scheduleRedraw();
      },
      shouldDrag: (currentPx: number) => {
        return onTimeRangeBoundary(currentPx) !== null;
      },
      onDrag: (
          dragStartPx: number,
          prevPx: number,
          currentPx: number,
          editing: boolean) => {
        const traceTime = globals.state.traceTime;
        const scale = frontendLocalState.timeScale;
        this.keepCurrentSelection = true;
        if (editing) {
          const startSec = frontendLocalState.selectedTimeRange.startSec;
          const endSec = frontendLocalState.selectedTimeRange.endSec;
          if (startSec !== undefined && endSec !== undefined) {
            const newTime = scale.pxToTime(currentPx - TRACK_SHELL_WIDTH);
            // Have to check again for when one boundary crosses over the other.
            const curBoundary = onTimeRangeBoundary(prevPx);
            if (curBoundary == null) return;
            const keepTime = curBoundary === 'START' ? endSec : startSec;
            frontendLocalState.selectTimeRange(
                Math.max(Math.min(keepTime, newTime), traceTime.startSec),
                Math.min(Math.max(keepTime, newTime), traceTime.endSec));
          }
        } else {
          frontendLocalState.setShowTimeSelectPreview(false);
          const dragStartTime = scale.pxToTime(dragStartPx - TRACK_SHELL_WIDTH);
          const dragEndTime = scale.pxToTime(currentPx - TRACK_SHELL_WIDTH);
          frontendLocalState.selectTimeRange(
              Math.max(
                  Math.min(dragStartTime, dragEndTime), traceTime.startSec),
              Math.min(Math.max(dragStartTime, dragEndTime), traceTime.endSec));
        }
        globals.rafScheduler.scheduleRedraw();
      }
    });
  }

  onremove() {
    window.removeEventListener('resize', this.onResize);
    if (this.zoomContent) this.zoomContent.shutdown();
  }

  view() {
    const scrollingPanels: AnyAttrsVnode[] =
        globals.state.scrollingTracks.map(id => m(TrackPanel, {key: id, id}));

    for (const group of Object.values(globals.state.trackGroups)) {
      scrollingPanels.push(m(TrackGroupPanel, {
        trackGroupId: group.id,
        key: `trackgroup-${group.id}`,
      }));
      if (group.collapsed) continue;
      for (const trackId of group.tracks) {
        scrollingPanels.push(m(TrackPanel, {
          key: `track-${group.id}-${trackId}`,
          id: trackId,
        }));
      }
    }
    scrollingPanels.unshift(m(QueryTable, {key: 'query'}));

    return m(
        '.page',
        m('.split-panel',
          m('.pan-and-zoom-content',
            {
              onclick: () => {
                // We don't want to deselect when panning/drag selecting.
                if (this.keepCurrentSelection) {
                  this.keepCurrentSelection = false;
                  return;
                }
                globals.makeSelection(Actions.deselect({}));
              }
            },
            m('.pinned-panel-container', m(PanelContainer, {
                doesScroll: false,
                panels: [
                  m(OverviewTimelinePanel, {key: 'overview'}),
                  m(TimeAxisPanel, {key: 'timeaxis'}),
                  m(TimeSelectionPanel, {key: 'timeselection'}),
                  m(NotesPanel, {key: 'notes'}),
                  m(TickmarkPanel, {key: 'searchTickmarks'}),
                  ...globals.state.pinnedTracks.map(
                      id => m(TrackPanel, {key: id, id})),
                ],
                kind: 'OVERVIEW',
              })),
            m('.scrolling-panel-container', m(PanelContainer, {
                doesScroll: true,
                panels: scrollingPanels,
                kind: 'TRACKS',
              }))),
          m('.video-panel',
            (globals.state.videoEnabled && globals.state.video != null) ?
                m(VideoPanel) :
                null)),
        m(DetailsPanel));
  }
}

export const ViewerPage = createPage({
  view() {
    return m(TraceViewer);
  }
});
