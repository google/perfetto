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
import {TimeSpan} from '../common/time';

import {TRACK_SHELL_WIDTH} from './css_constants';
import {DetailsPanel} from './details_panel';
import {globals} from './globals';
import {NotesPanel} from './notes_panel';
import {OverviewTimelinePanel} from './overview_timeline_panel';
import {createPage} from './pages';
import {PanAndZoomHandler} from './pan_and_zoom_handler';
import {AnyAttrsVnode, PanelContainer} from './panel_container';
import {TickmarkPanel} from './tickmark_panel';
import {TimeAxisPanel} from './time_axis_panel';
import {computeZoom} from './time_scale';
import {TimeSelectionPanel} from './time_selection_panel';
import {DISMISSED_PANNING_HINT_KEY} from './topbar';
import {TrackGroupPanel} from './track_group_panel';
import {TrackPanel} from './track_panel';
import {VideoPanel} from './video_panel';

const SIDEBAR_WIDTH = 256;

// Checks if the mousePos is within 3px of the start or end of the
// current selected time range.
function onTimeRangeBoundary(mousePos: number): 'START'|'END'|null {
  const selection = globals.state.currentSelection;
  if (selection !== null && selection.kind === 'AREA') {
    // If frontend selectedArea exists then we are in the process of editing the
    // time range and need to use that value instead.
    const area = globals.frontendLocalState.selectedArea ?
        globals.frontendLocalState.selectedArea :
        globals.state.areas[selection.areaId];
    const start = globals.frontendLocalState.timeScale.timeToPx(area.startSec);
    const end = globals.frontendLocalState.timeScale.timeToPx(area.endSec);
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
      frontendLocalState.updateLocalLimits(
          0,
          rect.width - TRACK_SHELL_WIDTH -
              frontendLocalState.getScrollbarWidth());
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
        // If the user has panned they no longer need the hint.
        localStorage.setItem(DISMISSED_PANNING_HINT_KEY, 'true');
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
      editSelection: (currentPx: number) => {
        return onTimeRangeBoundary(currentPx) !== null;
      },
      onSelection: (
          dragStartX: number,
          dragStartY: number,
          prevX: number,
          currentX: number,
          currentY: number,
          editing: boolean) => {
        const traceTime = globals.state.traceTime;
        const scale = frontendLocalState.timeScale;
        this.keepCurrentSelection = true;
        if (editing) {
          const selection = globals.state.currentSelection;
          if (selection !== null && selection.kind === 'AREA') {
            const area = globals.frontendLocalState.selectedArea ?
                globals.frontendLocalState.selectedArea :
                globals.state.areas[selection.areaId];
            let newTime = scale.pxToTime(currentX - TRACK_SHELL_WIDTH);
            // Have to check again for when one boundary crosses over the other.
            const curBoundary = onTimeRangeBoundary(prevX);
            if (curBoundary == null) return;
            const keepTime =
                curBoundary === 'START' ? area.endSec : area.startSec;
            // Don't drag selection outside of current screen.
            if (newTime < keepTime) {
              newTime = Math.max(newTime, scale.pxToTime(scale.startPx));
            } else {
              newTime = Math.min(newTime, scale.pxToTime(scale.endPx));
            }
            // When editing the time range we always use the saved tracks,
            // since these will not change.
            frontendLocalState.selectArea(
                Math.max(Math.min(keepTime, newTime), traceTime.startSec),
                Math.min(Math.max(keepTime, newTime), traceTime.endSec),
                globals.state.areas[selection.areaId].tracks);
          }
        } else {
          const startPx = Math.max(
              Math.min(dragStartX, currentX) - TRACK_SHELL_WIDTH,
              scale.startPx);
          const endPx = Math.min(
              Math.max(dragStartX, currentX) - TRACK_SHELL_WIDTH, scale.endPx);
          frontendLocalState.selectArea(
              scale.pxToTime(startPx), scale.pxToTime(endPx));
          frontendLocalState.areaY.start = dragStartY;
          frontendLocalState.areaY.end = currentY;
        }
        globals.rafScheduler.scheduleRedraw();
      },
      endSelection: (edit: boolean) => {
        globals.frontendLocalState.areaY.start = undefined;
        globals.frontendLocalState.areaY.end = undefined;
        const area = globals.frontendLocalState.selectedArea;
        // If we are editing we need to pass the current id through to ensure
        // the marked area with that id is also updated.
        if (edit) {
          const selection = globals.state.currentSelection;
          if (selection !== null && selection.kind === 'AREA' && area) {
            globals.dispatch(
                Actions.editArea({area, areaId: selection.areaId}));
          }
        } else if (area) {
          globals.makeSelection(Actions.selectArea({area}));
        }
        // Now the selection has ended we stored the final selected area in the
        // global state and can remove the in progress selection from the
        // frontendLocalState.
        globals.frontendLocalState.deselectArea();
        // Full redraw to color track shell.
        globals.rafScheduler.scheduleFullRedraw();
      }
    });
  }

  onremove() {
    window.removeEventListener('resize', this.onResize);
    if (this.zoomContent) this.zoomContent.shutdown();
  }

  view() {
    const scrollingPanels: AnyAttrsVnode[] = globals.state.scrollingTracks.map(
        id => m(TrackPanel, {key: id, id, selectable: true}));

    for (const group of Object.values(globals.state.trackGroups)) {
      scrollingPanels.push(m(TrackGroupPanel, {
        trackGroupId: group.id,
        key: `trackgroup-${group.id}`,
        selectable: true,
      }));
      if (group.collapsed) continue;
      // The first track is the summary track, and is displayed as part of the
      // group panel, we don't want to display it twice so we start from 1.
      for (let i = 1; i < group.tracks.length; ++i) {
        const id = group.tracks[i];
        scrollingPanels.push(m(TrackPanel, {
          key: `track-${group.id}-${id}`,
          id,
          selectable: true,
        }));
      }
    }

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
                      id => m(TrackPanel, {key: id, id, selectable: true})),
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
