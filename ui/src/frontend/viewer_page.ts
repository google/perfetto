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

import {findRef, toHTMLElement} from '../base/dom_utils';
import {clamp} from '../base/math_utils';
import {Time} from '../base/time';
import {Actions} from '../common/actions';
import {TrackCacheEntry} from '../common/track_cache';
import {featureFlags} from '../core/feature_flags';
import {raf} from '../core/raf_scheduler';
import {TrackTags} from '../public';

import {TRACK_SHELL_WIDTH} from './css_constants';
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
import {TrackPanel, getTitleFontSize} from './track_panel';
import {assertExists} from '../base/logging';
import {PxSpan, TimeScale} from './time_scale';
import {TrackGroupState} from '../common/state';
import {FuzzyFinder, fuzzyMatch, FuzzySegment} from '../base/fuzzy';
import {exists} from '../base/utils';
import {EmptyState} from '../widgets/empty_state';
import {removeFalsyValues} from '../base/array_utils';

const OVERVIEW_PANEL_FLAG = featureFlags.register({
  id: 'overviewVisible',
  name: 'Overview Panel',
  description: 'Show the panel providing an overview of the trace',
  defaultValue: true,
});

// Checks if the mousePos is within 3px of the start or end of the
// current selected time range.
function onTimeRangeBoundary(
  timescale: TimeScale,
  mousePos: number,
): 'START' | 'END' | null {
  const selection = globals.state.selection;
  if (selection.kind === 'area') {
    // If frontend selectedArea exists then we are in the process of editing the
    // time range and need to use that value instead.
    const area = globals.timeline.selectedArea
      ? globals.timeline.selectedArea
      : selection;
    const start = timescale.timeToPx(area.start);
    const end = timescale.timeToPx(area.end);
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
  private zoomContent?: PanAndZoomHandler;
  // Used to prevent global deselection if a pan/drag select occurred.
  private keepCurrentSelection = false;

  private overviewTimelinePanel = new OverviewTimelinePanel();
  private timeAxisPanel = new TimeAxisPanel();
  private timeSelectionPanel = new TimeSelectionPanel();
  private notesPanel = new NotesPanel();
  private tickmarkPanel = new TickmarkPanel();
  private timelineWidthPx?: number;

  private readonly PAN_ZOOM_CONTENT_REF = 'pan-and-zoom-content';

  oncreate(vnode: m.CVnodeDOM) {
    const panZoomElRaw = findRef(vnode.dom, this.PAN_ZOOM_CONTENT_REF);
    const panZoomEl = toHTMLElement(assertExists(panZoomElRaw));

    this.zoomContent = new PanAndZoomHandler({
      element: panZoomEl,
      onPanned: (pannedPx: number) => {
        const timeline = globals.timeline;

        if (this.timelineWidthPx === undefined) return;

        this.keepCurrentSelection = true;
        const timescale = new TimeScale(
          timeline.visibleWindow,
          new PxSpan(0, this.timelineWidthPx),
        );
        const tDelta = timescale.pxToDuration(pannedPx);
        timeline.panVisibleWindow(tDelta);

        // If the user has panned they no longer need the hint.
        localStorage.setItem(DISMISSED_PANNING_HINT_KEY, 'true');
        raf.scheduleRedraw();
      },
      onZoomed: (zoomedPositionPx: number, zoomRatio: number) => {
        const timeline = globals.timeline;
        // TODO(hjd): Avoid hardcoding TRACK_SHELL_WIDTH.
        // TODO(hjd): Improve support for zooming in overview timeline.
        const zoomPx = zoomedPositionPx - TRACK_SHELL_WIDTH;
        const rect = vnode.dom.getBoundingClientRect();
        const centerPoint = zoomPx / (rect.width - TRACK_SHELL_WIDTH);
        timeline.zoomVisibleWindow(1 - zoomRatio, centerPoint);
        raf.scheduleRedraw();
      },
      editSelection: (currentPx: number) => {
        if (this.timelineWidthPx === undefined) return false;
        const timescale = new TimeScale(
          globals.timeline.visibleWindow,
          new PxSpan(0, this.timelineWidthPx),
        );
        return onTimeRangeBoundary(timescale, currentPx) !== null;
      },
      onSelection: (
        dragStartX: number,
        dragStartY: number,
        prevX: number,
        currentX: number,
        currentY: number,
        editing: boolean,
      ) => {
        const traceTime = globals.traceContext;
        const timeline = globals.timeline;

        if (this.timelineWidthPx === undefined) return;

        // TODO(stevegolton): Don't get the windowSpan from globals, get it from
        // here!
        const {visibleWindow} = timeline;
        const timespan = visibleWindow.toTimeSpan();
        this.keepCurrentSelection = true;

        const timescale = new TimeScale(
          timeline.visibleWindow,
          new PxSpan(0, this.timelineWidthPx),
        );

        if (editing) {
          const selection = globals.state.selection;
          if (selection.kind === 'area') {
            const area = globals.timeline.selectedArea
              ? globals.timeline.selectedArea
              : selection;
            let newTime = timescale
              .pxToHpTime(currentX - TRACK_SHELL_WIDTH)
              .toTime();
            // Have to check again for when one boundary crosses over the other.
            const curBoundary = onTimeRangeBoundary(timescale, prevX);
            if (curBoundary == null) return;
            const keepTime = curBoundary === 'START' ? area.end : area.start;
            // Don't drag selection outside of current screen.
            if (newTime < keepTime) {
              newTime = Time.max(newTime, timespan.start);
            } else {
              newTime = Time.min(newTime, timespan.end);
            }
            // When editing the time range we always use the saved tracks,
            // since these will not change.
            timeline.selectArea(
              Time.max(Time.min(keepTime, newTime), traceTime.start),
              Time.min(Time.max(keepTime, newTime), traceTime.end),
              selection.tracks,
            );
          }
        } else {
          let startPx = Math.min(dragStartX, currentX) - TRACK_SHELL_WIDTH;
          let endPx = Math.max(dragStartX, currentX) - TRACK_SHELL_WIDTH;
          if (startPx < 0 && endPx < 0) return;
          startPx = clamp(startPx, 0, this.timelineWidthPx);
          endPx = clamp(endPx, 0, this.timelineWidthPx);
          timeline.selectArea(
            timescale.pxToHpTime(startPx).toTime('floor'),
            timescale.pxToHpTime(endPx).toTime('ceil'),
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
          const selection = globals.state.selection;
          if (selection.kind === 'area' && area) {
            globals.dispatch(Actions.selectArea({...area}));
          }
        } else if (area) {
          globals.makeSelection(Actions.selectArea({...area}));
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
    if (this.zoomContent) this.zoomContent[Symbol.dispose]();
  }

  view() {
    const scrollingPanels = renderToplevelPanels(globals.state.trackFilterTerm);

    const result = m(
      '.page.viewer-page',
      m(
        '.pan-and-zoom-content',
        {
          ref: this.PAN_ZOOM_CONTENT_REF,
          onclick: () => {
            // We don't want to deselect when panning/drag selecting.
            if (this.keepCurrentSelection) {
              this.keepCurrentSelection = false;
              return;
            }
            globals.clearSelection();
          },
        },
        m(
          '.pf-timeline-header',
          m(PanelContainer, {
            className: 'header-panel-container',
            panels: removeFalsyValues([
              OVERVIEW_PANEL_FLAG.get() && this.overviewTimelinePanel,
              this.timeAxisPanel,
              this.timeSelectionPanel,
              this.notesPanel,
              this.tickmarkPanel,
            ]),
          }),
          m('.scrollbar-spacer-vertical'),
        ),
        m(PanelContainer, {
          className: 'pinned-panel-container',
          panels: globals.state.pinnedTracks.map((key) => {
            const trackBundle = resolveTrack(key);
            return new TrackPanel({
              trackKey: key,
              title: trackBundle.title,
              tags: trackBundle.tags,
              trackFSM: trackBundle.trackFSM,
              revealOnCreate: true,
              closeable: trackBundle.closeable,
              chips: trackBundle.chips,
              pluginId: trackBundle.pluginId,
            });
          }),
        }),
        scrollingPanels.length === 0 &&
          filterTermIsValid(globals.state.trackFilterTerm)
          ? m(
              EmptyState,
              {title: 'No matching tracks'},
              `No tracks match filter term "${globals.state.trackFilterTerm}"`,
            )
          : m(PanelContainer, {
              className: 'scrolling-panel-container',
              panels: scrollingPanels,
              onPanelStackResize: (width) => {
                const timelineWidth = width - TRACK_SHELL_WIDTH;
                this.timelineWidthPx = timelineWidth;
              },
            }),
      ),
      m(TabPanel),
    );

    globals.trackManager.flushOldTracks();
    return result;
  }
}

// Given a set of fuzzy matched results, render the matching segments in bold
function renderFuzzyMatchedTrackTitle(title: FuzzySegment[]): m.Children {
  return title.map(({matching, value}) => {
    return matching ? m('b', value) : value;
  });
}

function filterTermIsValid(
  filterTerm: undefined | string,
): filterTerm is string {
  // Note: Boolean(filterTerm) returns the same result, but this is clearer
  return filterTerm !== undefined && filterTerm !== '';
}

// Split filter term on commas into a list of tokens, cleaning up any whitespace
// before and after the token and removing any blank tokens
function tokenizeFilterTerm(term: string): ReadonlyArray<string> {
  return term
    .split(',')
    .map((token) => token.trim())
    .filter((token) => token.length > 0);
}

// Render the toplevel "scrolling" tracks and track groups
function renderToplevelPanels(filterTerm: string | undefined): PanelOrGroup[] {
  const scrollingPanels: PanelOrGroup[] = renderTrackPanels(
    globals.state.scrollingTracks,
    filterTerm,
  );

  for (const group of Object.values(globals.state.trackGroups)) {
    if (filterTermIsValid(filterTerm)) {
      const tokens = tokenizeFilterTerm(filterTerm);
      // Match group names that match any of the tokens
      const result = fuzzyMatch(group.name, ...tokens);
      if (result.matches) {
        // If the group name matches, render the entire group as normal
        const title = renderFuzzyMatchedTrackTitle(result.segments);
        scrollingPanels.push({
          kind: 'group',
          collapsed: group.collapsed,
          childPanels: group.collapsed ? [] : renderTrackPanels(group.tracks),
          header: renderTrackGroupPanel(group, true, group.collapsed, title),
        });
      } else {
        // If we are filtering, render the group header only if it contains
        // matching tracks
        const childPanels = renderTrackPanels(group.tracks, filterTerm);
        if (childPanels.length === 0) continue;
        scrollingPanels.push({
          kind: 'group',
          collapsed: false,
          childPanels,
          header: renderTrackGroupPanel(group, false, false),
        });
      }
    } else {
      // Always render the group header, but only render child tracks if not
      // collapsed
      scrollingPanels.push({
        kind: 'group',
        collapsed: group.collapsed,
        childPanels: group.collapsed ? [] : renderTrackPanels(group.tracks),
        header: renderTrackGroupPanel(group, true, group.collapsed),
      });
    }
  }

  return scrollingPanels;
}

// Given a list of tracks and a filter term, return a list pf panels filtered by
// the filter term
function renderTrackPanels(trackKeys: string[], filterTerm?: string): Panel[] {
  if (filterTermIsValid(filterTerm)) {
    const tokens = tokenizeFilterTerm(filterTerm);
    const matcher = new FuzzyFinder(trackKeys, (key) => {
      return globals.state.tracks[key].name;
    });
    // Filter tracks which match any of the tokens
    const filtered = matcher.find(...tokens);
    return filtered.map(({item: key, segments}) => {
      return renderTrackPanel(key, renderFuzzyMatchedTrackTitle(segments));
    });
  } else {
    // No point in applying any filtering...
    return trackKeys.map((key) => {
      return renderTrackPanel(key);
    });
  }
}

function renderTrackPanel(key: string, title?: m.Children) {
  const trackBundle = resolveTrack(key);
  return new TrackPanel({
    trackKey: key,
    title: m(
      'span',
      {
        style: {
          'font-size': getTitleFontSize(trackBundle.title),
        },
      },
      Boolean(title) ? title : trackBundle.title,
    ),
    tags: trackBundle.tags,
    trackFSM: trackBundle.trackFSM,
    closeable: trackBundle.closeable,
    chips: trackBundle.chips,
    pluginId: trackBundle.pluginId,
  });
}

function renderTrackGroupPanel(
  group: TrackGroupState,
  collapsable: boolean,
  collapsed: boolean,
  title?: m.Children,
): TrackGroupPanel {
  const summaryTrackKey = group.summaryTrack;

  if (exists(summaryTrackKey)) {
    const trackBundle = resolveTrack(summaryTrackKey);
    return new TrackGroupPanel({
      groupKey: group.key,
      trackFSM: trackBundle.trackFSM,
      subtitle: trackBundle.subtitle,
      tags: trackBundle.tags,
      chips: trackBundle.chips,
      collapsed,
      title: exists(title) ? title : group.name,
      tooltip: group.name,
      collapsable,
    });
  } else {
    return new TrackGroupPanel({
      groupKey: group.key,
      collapsed,
      title: exists(title) ? title : group.name,
      tooltip: group.name,
      collapsable,
    });
  }
}

// Resolve a track and its metadata through the track cache
function resolveTrack(key: string): TrackBundle {
  const trackState = globals.state.tracks[key];
  const {uri, name, closeable} = trackState;
  const trackDesc = globals.trackManager.resolveTrackInfo(uri);
  const trackCacheEntry =
    trackDesc && globals.trackManager.resolveTrack(key, trackDesc);
  const trackFSM = trackCacheEntry;
  const tags = trackCacheEntry?.desc.tags;
  const subtitle = trackCacheEntry?.desc.subtitle;
  const chips = trackCacheEntry?.desc.chips;
  const plugin = trackCacheEntry?.desc.pluginId;
  return {
    title: name,
    subtitle,
    closeable: closeable ?? false,
    tags,
    trackFSM,
    chips,
    pluginId: plugin,
  };
}

interface TrackBundle {
  readonly title: string;
  readonly subtitle?: string;
  readonly closeable: boolean;
  readonly trackFSM?: TrackCacheEntry;
  readonly tags?: TrackTags;
  readonly chips?: ReadonlyArray<string>;
  readonly pluginId?: string;
}

export const ViewerPage = createPage({
  view() {
    return m(TraceViewer);
  },
});
