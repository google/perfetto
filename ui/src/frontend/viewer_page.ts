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

import m from 'mithril';

import {clamp} from '../base/math_utils';
import {Time} from '../base/time';
import {Actions} from '../common/actions';
import {featureFlags} from '../common/feature_flags';
import {raf} from '../core/raf_scheduler';

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
import {TimeSelectionPanel} from './time_selection_panel';
import {DISMISSED_PANNING_HINT_KEY} from './topbar';
import {TrackGroupPanel} from './track_group_panel';
import {TrackPanel} from './track_panel';

const OVERVIEW_PANEL_FLAG = featureFlags.register({
  id: 'overviewVisible',
  name: 'Overview Panel',
  description: 'Show the panel providing an overview of the trace',
  defaultValue: true,
});

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
    const {visibleTimeScale} = globals.frontendLocalState;
    const start = visibleTimeScale.timeToPx(area.start);
    const end = visibleTimeScale.timeToPx(area.end);
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

export interface TrackGroupAttrs {
  header: AnyAttrsVnode;
  collapsed: boolean;
  childTracks: AnyAttrsVnode[];
}

export class TrackGroup implements m.ClassComponent<TrackGroupAttrs> {
  view() {
    // TrackGroup component acts as a holder for a bunch of tracks rendered
    // together: the actual rendering happens in PanelContainer. In order to
    // avoid confusion, this method remains empty.
  }
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
      raf.scheduleFullRedraw();
    };

    // Once ResizeObservers are out, we can stop accessing the window here.
    window.addEventListener('resize', this.onResize);

    const panZoomEl =
        vnode.dom.querySelector('.pan-and-zoom-content') as HTMLElement;

    this.zoomContent = new PanAndZoomHandler({
      element: panZoomEl,
      onPanned: (pannedPx: number) => {
        const {
          visibleTimeScale,
        } = globals.frontendLocalState;

        this.keepCurrentSelection = true;
        const tDelta = visibleTimeScale.pxDeltaToDuration(pannedPx);
        frontendLocalState.panVisibleWindow(tDelta);

        // If the user has panned they no longer need the hint.
        localStorage.setItem(DISMISSED_PANNING_HINT_KEY, 'true');
        raf.scheduleRedraw();
      },
      onZoomed: (zoomedPositionPx: number, zoomRatio: number) => {
        // TODO(hjd): Avoid hardcoding TRACK_SHELL_WIDTH.
        // TODO(hjd): Improve support for zooming in overview timeline.
        const zoomPx = zoomedPositionPx - TRACK_SHELL_WIDTH;
        const rect = vnode.dom.getBoundingClientRect();
        const centerPoint = zoomPx / (rect.width - TRACK_SHELL_WIDTH);
        frontendLocalState.zoomVisibleWindow(1 - zoomRatio, centerPoint);
        raf.scheduleRedraw();
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
        const {visibleTimeScale} = frontendLocalState;
        this.keepCurrentSelection = true;
        if (editing) {
          const selection = globals.state.currentSelection;
          if (selection !== null && selection.kind === 'AREA') {
            const area = globals.frontendLocalState.selectedArea ?
                globals.frontendLocalState.selectedArea :
                globals.state.areas[selection.areaId];
            let newTime =
                visibleTimeScale.pxToHpTime(currentX - TRACK_SHELL_WIDTH)
                    .toTime();
            // Have to check again for when one boundary crosses over the other.
            const curBoundary = onTimeRangeBoundary(prevX);
            if (curBoundary == null) return;
            const keepTime = curBoundary === 'START' ? area.end : area.start;
            // Don't drag selection outside of current screen.
            if (newTime < keepTime) {
              newTime =
                  Time.max(newTime, visibleTimeScale.timeSpan.start.toTime());
            } else {
              newTime =
                  Time.max(newTime, visibleTimeScale.timeSpan.end.toTime());
            }
            // When editing the time range we always use the saved tracks,
            // since these will not change.
            frontendLocalState.selectArea(
                Time.max(Time.min(keepTime, newTime), traceTime.start),
                Time.min(Time.max(keepTime, newTime), traceTime.end),
                globals.state.areas[selection.areaId].tracks);
          }
        } else {
          let startPx = Math.min(dragStartX, currentX) - TRACK_SHELL_WIDTH;
          let endPx = Math.max(dragStartX, currentX) - TRACK_SHELL_WIDTH;
          if (startPx < 0 && endPx < 0) return;
          const {pxSpan} = visibleTimeScale;
          startPx = clamp(startPx, pxSpan.start, pxSpan.end);
          endPx = clamp(endPx, pxSpan.start, pxSpan.end);
          frontendLocalState.selectArea(
              visibleTimeScale.pxToHpTime(startPx).toTime('floor'),
              visibleTimeScale.pxToHpTime(endPx).toTime('ceil'),
          );
          frontendLocalState.areaY.start = dragStartY;
          frontendLocalState.areaY.end = currentY;
        }
        raf.scheduleRedraw();
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
        raf.scheduleFullRedraw();
      },
    });
  }

  onremove() {
    window.removeEventListener('resize', this.onResize);
    if (this.zoomContent) this.zoomContent.dispose();
  }

  view() {
    const scrollingPanels: AnyAttrsVnode[] = globals.state.scrollingTracks.map(
        (key) => m(TrackPanel, {key, trackKey: key, selectable: true}));

    for (const group of Object.values(globals.state.trackGroups)) {
      const headerPanel = m(TrackGroupPanel, {
        trackGroupId: group.id,
        key: `trackgroup-${group.id}`,
        selectable: true,
      });

      const childTracks: AnyAttrsVnode[] = [];
      // The first track is the summary track, and is displayed as part of the
      // group panel, we don't want to display it twice so we start from 1.
      if (!group.collapsed) {
        for (let i = 1; i < group.tracks.length; ++i) {
          const id = group.tracks[i];
          childTracks.push(m(TrackPanel, {
            key: `track-${group.id}-${id}`,
            trackKey: id,
            selectable: true,
          }));
        }
      }
      scrollingPanels.push(m(TrackGroup, {
        header: headerPanel,
        collapsed: group.collapsed,
        childTracks,
      } as TrackGroupAttrs));
    }

    const overviewPanel = [];
    if (OVERVIEW_PANEL_FLAG.get()) {
      overviewPanel.push(m(OverviewTimelinePanel, {key: 'overview'}));
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
              },
            },
            m('.pinned-panel-container', m(PanelContainer, {
                doesScroll: false,
                panels: [
                  ...overviewPanel,
                  m(TimeAxisPanel, {key: 'timeaxis'}),
                  m(TimeSelectionPanel, {key: 'timeselection'}),
                  m(NotesPanel, {key: 'notes'}),
                  m(TickmarkPanel, {key: 'searchTickmarks'}),
                  ...globals.state.pinnedTracks.map(
                      (id) =>
                          m(TrackPanel,
                            {key: id, trackKey: id, selectable: true})),
                ],
                kind: 'OVERVIEW',
              })),
            m('.scrolling-panel-container', m(PanelContainer, {
                doesScroll: true,
                panels: scrollingPanels,
                kind: 'TRACKS',
              })))),
        m(DetailsPanel));
  }
}

export const ViewerPage = createPage({
  view() {
    return m(TraceViewer);
  },
});
