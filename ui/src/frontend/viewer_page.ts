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

import {getScrollbarWidth} from '../base/dom_utils';
import {clamp} from '../base/math_utils';
import {Time} from '../base/time';
import {Actions} from '../common/actions';
import {TrackCacheEntry} from '../common/track_cache';
import {TABS_V2_FLAG, featureFlags} from '../core/feature_flags';
import {raf} from '../core/raf_scheduler';
import {TrackTags} from '../public';

import {TRACK_SHELL_WIDTH} from './css_constants';
import {DetailsPanel} from './details_panel';
import {globals} from './globals';
import {NotesPanel} from './notes_panel';
import {OverviewTimelinePanel} from './overview_timeline_panel';
import {createPage} from './pages';
import {PanAndZoomHandler} from './pan_and_zoom_handler';
import {Panel, PanelContainer, PanelOrGroup} from './panel_container';
import {publishShowPanningHint} from './publish';
import {TabPanel} from './tab_panel';
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
    const area = globals.timeline.selectedArea ?
      globals.timeline.selectedArea :
      globals.state.areas[selection.areaId];
    const {visibleTimeScale} = globals.timeline;
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

/**
 * Top-most level component for the viewer page. Holds tracks, brush timeline,
 * panels, and everything else that's part of the main trace viewer page.
 */
class TraceViewer implements m.ClassComponent {
  private onResize: () => void = () => {};
  private zoomContent?: PanAndZoomHandler;
  // Used to prevent global deselection if a pan/drag select occurred.
  private keepCurrentSelection = false;

  private overviewTimelinePanel = new OverviewTimelinePanel('overview');
  private timeAxisPanel = new TimeAxisPanel('timeaxis');
  private timeSelectionPanel = new TimeSelectionPanel('timeselection');
  private notesPanel = new NotesPanel('notes');
  private tickmarkPanel = new TickmarkPanel('searchTickmarks');

  oncreate(vnode: m.CVnodeDOM) {
    const timeline = globals.timeline;
    const updateDimensions = () => {
      const rect = vnode.dom.getBoundingClientRect();
      timeline.updateLocalLimits(
        0, rect.width - TRACK_SHELL_WIDTH - getScrollbarWidth());
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
        } = globals.timeline;

        this.keepCurrentSelection = true;
        const tDelta = visibleTimeScale.pxDeltaToDuration(pannedPx);
        timeline.panVisibleWindow(tDelta);

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
        timeline.zoomVisibleWindow(1 - zoomRatio, centerPoint);
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
        const {visibleTimeScale} = timeline;
        this.keepCurrentSelection = true;
        if (editing) {
          const selection = globals.state.currentSelection;
          if (selection !== null && selection.kind === 'AREA') {
            const area = globals.timeline.selectedArea ?
              globals.timeline.selectedArea :
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
            timeline.selectArea(
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
          timeline.selectArea(
            visibleTimeScale.pxToHpTime(startPx).toTime('floor'),
            visibleTimeScale.pxToHpTime(endPx).toTime('ceil'),
          );
          timeline.areaY.start = dragStartY;
          timeline.areaY.end = currentY;
          publishShowPanningHint();
        }
        raf.scheduleRedraw();
      },
      endSelection: (edit: boolean) => {
        globals.timeline.areaY.start = undefined;
        globals.timeline.areaY.end = undefined;
        const area = globals.timeline.selectedArea;
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
        // timeline.
        globals.timeline.deselectArea();
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
    const scrollingPanels: PanelOrGroup[] =
        globals.state.scrollingTracks.map((key) => {
          const trackBundle = this.resolveTrack(key);
          return new TrackPanel({
            key,
            trackKey: key,
            title: trackBundle.title,
            tags: trackBundle.tags,
            trackFSM: trackBundle.trackFSM,
          });
        });

    for (const group of Object.values(globals.state.trackGroups)) {
      const key = group.tracks[0];
      const trackBundle = this.resolveTrack(key);
      const headerPanel = new TrackGroupPanel({
        trackGroupId: group.id,
        key: `trackgroup-${group.id}`,
        trackFSM: trackBundle.trackFSM,
        labels: trackBundle.labels,
        tags: trackBundle.tags,
        collapsed: group.collapsed,
        title: group.name,
      });

      const childTracks: Panel[] = [];
      // The first track is the summary track, and is displayed as part of the
      // group panel, we don't want to display it twice so we start from 1.
      if (!group.collapsed) {
        for (let i = 1; i < group.tracks.length; ++i) {
          const key = group.tracks[i];
          const trackBundle = this.resolveTrack(key);
          const panel = new TrackPanel({
            key: `track-${group.id}-${key}`,
            trackKey: key,
            title: trackBundle.title,
            tags: trackBundle.tags,
            trackFSM: trackBundle.trackFSM,
          });
          childTracks.push(panel);
        }
      }

      scrollingPanels.push({
        kind: 'group',
        collapsed: group.collapsed,
        childTracks,
        header: headerPanel,
        trackGroupId: group.id,
      });
    }

    const overviewPanel = [];
    if (OVERVIEW_PANEL_FLAG.get()) {
      overviewPanel.push(this.overviewTimelinePanel);
    }

    const result = m(
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
          m(PanelContainer, {
            className: 'header-panel-container',
            doesScroll: false,
            panels: [
              ...overviewPanel,
              this.timeAxisPanel,
              this.timeSelectionPanel,
              this.notesPanel,
              this.tickmarkPanel,
            ],
            kind: 'OVERVIEW',
          }),
          m(PanelContainer, {
            className: 'pinned-panel-container',
            doesScroll: true,
            panels: globals.state.pinnedTracks.map((key) => {
              const trackBundle = this.resolveTrack(key);
              return new TrackPanel({
                key,
                trackKey: key,
                title: trackBundle.title,
                tags: trackBundle.tags,
                trackFSM: trackBundle.trackFSM,
                revealOnCreate: true,
              });
            }),
            kind: 'TRACKS',
          }),
          m(PanelContainer, {
            className: 'scrolling-panel-container',
            doesScroll: true,
            panels: scrollingPanels,
            kind: 'TRACKS',
          }),
        ),
      ),
      this.renderTabPanel());

    globals.trackManager.flushOldTracks();
    return result;
  }

  // Resolve a track and its metadata through the track cache
  private resolveTrack(key: string): TrackBundle {
    const trackState = globals.state.tracks[key];
    const {uri, params, name, labels} = trackState;
    const trackDesc = globals.trackManager.resolveTrackInfo(uri);
    const trackCacheEntry =
        trackDesc && globals.trackManager.resolveTrack(key, trackDesc, params);
    const trackFSM = trackCacheEntry;
    const tags = trackCacheEntry?.desc.tags;
    const trackIds = trackCacheEntry?.desc.trackIds;
    return {
      title: name,
      tags,
      trackFSM,
      labels,
      trackIds,
    };
  }

  private renderTabPanel() {
    if (TABS_V2_FLAG.get()) {
      return m(TabPanel);
    } else {
      return m(DetailsPanel);
    }
  }
}

interface TrackBundle {
  title: string;
  trackFSM?: TrackCacheEntry;
  tags?: TrackTags;
  labels?: string[];
  trackIds?: number[];
}

export const ViewerPage = createPage({
  view() {
    return m(TraceViewer);
  },
});
