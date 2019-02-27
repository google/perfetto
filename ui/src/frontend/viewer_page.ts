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

import {copyToClipboard} from './clipboard';
import {DragGestureHandler} from './drag_gesture_handler';
import {globals} from './globals';
import {NotesEditorPanel, NotesPanel} from './notes_panel';
import {SliceDetailsPanel} from './slice_panel';
import {OverviewTimelinePanel} from './overview_timeline_panel';
import {createPage} from './pages';
import {PanAndZoomHandler} from './pan_and_zoom_handler';
import {Panel} from './panel';
import {AnyAttrsVnode, PanelContainer} from './panel_container';
import {TimeAxisPanel} from './time_axis_panel';
import {computeZoom} from './time_scale';
import {TimeSelectionPanel} from './time_selection_panel';
import {TRACK_SHELL_WIDTH} from './track_constants';
import {TrackGroupPanel} from './track_group_panel';
import {TrackPanel} from './track_panel';
import {Actions} from '../common/actions';

const DRAG_HANDLE_HEIGHT_PX = 12;
const DEFAULT_DETAILS_HEIGHT_PX = 230 + DRAG_HANDLE_HEIGHT_PX;

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

interface DragHandleAttrs {
  height: number;
  resize: (height: number) => void;
}

class DragHandle implements m.ClassComponent<DragHandleAttrs> {
  private dragStartHeight = 0;
  private height = 0;
  private resize: undefined|((height: number) => void);

  oncreate({dom, attrs}: m.CVnodeDOM<DragHandleAttrs>) {
    this.resize = attrs.resize;
    this.height = attrs.height;
    const elem = dom as HTMLElement;
    new DragGestureHandler(
        elem,
        this.onDrag.bind(this),
        this.onDragStart.bind(this),
        this.onDragEnd.bind(this));
  }

  onupdate({attrs}: m.CVnodeDOM<DragHandleAttrs>) {
    this.resize = attrs.resize;
    this.height = attrs.height;
  }

  onDrag(_x: number, y: number) {
    if (this.resize) {
      const newHeight = this.dragStartHeight + (DRAG_HANDLE_HEIGHT_PX / 2) - y;
      this.resize(Math.floor(newHeight));
    }
    globals.rafScheduler.scheduleFullRedraw();
  }

  onDragStart(_x: number, _y: number) {
    this.dragStartHeight = this.height;
  }

  onDragEnd() {}

  view() {
    return m('.handle');
  }
}

/**
 * Top-most level component for the viewer page. Holds tracks, brush timeline,
 * panels, and everything else that's part of the main trace viewer page.
 */
class TraceViewer implements m.ClassComponent {
  private onResize: () => void = () => {};
  private zoomContent?: PanAndZoomHandler;
  private detailsHeight = DRAG_HANDLE_HEIGHT_PX;
  // Used to set details panel to default height on selection.
  private showDetailsPanel = false;
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
      contentOffsetX: TRACK_SHELL_WIDTH,
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
      onDragSelect: (selectStartPx: number|null, selectEndPx: number) => {
        if (!selectStartPx) return;
        this.keepCurrentSelection = true;
        globals.frontendLocalState.setShowTimeSelectPreview(false);
        const traceTime = globals.state.traceTime;
        const scale = frontendLocalState.timeScale;
        const startPx = Math.min(selectStartPx, selectEndPx);
        const endPx = Math.max(selectStartPx, selectEndPx);
        const startTs = Math.max(traceTime.startSec,
                               scale.pxToTime(startPx - TRACK_SHELL_WIDTH));
        const endTs = Math.min(traceTime.endSec,
                               scale.pxToTime(endPx - TRACK_SHELL_WIDTH));
        globals.dispatch(Actions.selectTimeSpan({startTs, endTs}));
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
    scrollingPanels.unshift(m(QueryTable));

    const detailsPanels: AnyAttrsVnode[] = [];
    if (globals.state.currentSelection) {
      if (!this.showDetailsPanel &&
          globals.state.currentSelection.kind !== 'TIMESPAN') {
        this.detailsHeight = DEFAULT_DETAILS_HEIGHT_PX;
        this.showDetailsPanel = true;
      }
      switch (globals.state.currentSelection.kind) {
        case 'NOTE':
          detailsPanels.push(m(NotesEditorPanel, {
            key: 'notes',
            id: globals.state.currentSelection.id,
          }));
          break;
        case 'SLICE':
          detailsPanels.push(m(SliceDetailsPanel, {
            key: 'slice',
            utid: globals.state.currentSelection.utid,
          }));
          break;
        default:
          break;
      }
    } else {
      // No current selection so hide the details panel.
      if (this.showDetailsPanel) {
        this.showDetailsPanel = false;
        this.detailsHeight = DRAG_HANDLE_HEIGHT_PX;
      }
    }

    return m(
        '.page',
        m('.pan-and-zoom-content', {
          onclick: () =>
          {
            // We don't want to deselect when panning/drag selecting.
            if (this.keepCurrentSelection) {
              this.keepCurrentSelection = false;
              return;
            }
            globals.dispatch(Actions.deselect({}));
          }},
          m('.pinned-panel-container', m(PanelContainer, {
              doesScroll: false,
              panels: [
                m(OverviewTimelinePanel, {key: 'overview'}),
                m(TimeAxisPanel, {key: 'timeaxis'}),
                m(TimeSelectionPanel, {key: 'timeselection'}),
                m(NotesPanel, {key: 'notes'}),
                ...globals.state.pinnedTracks.map(
                    id => m(TrackPanel, {key: id, id})),
              ],
            })),
          m('.scrolling-panel-container', m(PanelContainer, {
              doesScroll: true,
              panels: scrollingPanels,
            }))),
        m('.details-content',
          {style: {height: `${this.detailsHeight}px`}},
          m(DragHandle, {
            resize: (height: number) => {
              this.detailsHeight = Math.max(height, DRAG_HANDLE_HEIGHT_PX);
            },
            height: this.detailsHeight,
          }),
          m('.details-panel-container', m(PanelContainer,
            {doesScroll: true, panels: detailsPanels}))));
  }
}

export const ViewerPage = createPage({
  view() {
    return m(TraceViewer);
  }
});
