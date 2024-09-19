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
import {featureFlags} from '../core/feature_flags';
import {raf} from '../core/raf_scheduler';
import {TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {NotesPanel} from './notes_panel';
import {OverviewTimelinePanel} from './overview_timeline_panel';
import {createPage} from './pages';
import {PanAndZoomHandler} from './pan_and_zoom_handler';
import {
  PanelContainer,
  PanelOrGroup,
  RenderedPanelInfo,
} from './panel_container';
import {publishShowPanningHint} from './publish';
import {TabPanel} from './tab_panel';
import {TickmarkPanel} from './tickmark_panel';
import {TimeAxisPanel} from './time_axis_panel';
import {TimeSelectionPanel} from './time_selection_panel';
import {DISMISSED_PANNING_HINT_KEY} from './topbar';
import {TrackGroupPanel} from './track_group_panel';
import {TrackPanel} from './track_panel';
import {assertExists} from '../base/logging';
import {TimeScale} from '../base/time_scale';
import {GroupNode, Node, TrackNode} from '../public/workspace';
import {fuzzyMatch} from '../base/fuzzy';
import {Optional} from '../base/utils';
import {EmptyState} from '../widgets/empty_state';
import {removeFalsyValues} from '../base/array_utils';
import {renderFlows} from './flow_events_renderer';
import {Size2D} from '../base/geom';
import {canvasClip, canvasSave} from '../base/canvas_utils';

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
  const selection = globals.selectionManager.selection;
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
        const timescale = new TimeScale(timeline.visibleWindow, {
          left: 0,
          right: this.timelineWidthPx,
        });
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
        const timescale = new TimeScale(globals.timeline.visibleWindow, {
          left: 0,
          right: this.timelineWidthPx,
        });
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

        const timescale = new TimeScale(timeline.visibleWindow, {
          left: 0,
          right: this.timelineWidthPx,
        });

        if (editing) {
          const selection = globals.selectionManager.selection;
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
              selection.trackUris,
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
          const selection = globals.selectionManager.selection;
          if (selection.kind === 'area' && area) {
            globals.selectionManager.setArea({...area});
          }
        } else if (area) {
          globals.selectionManager.setArea({...area});
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
            globals.selectionManager.clear();
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
          panels: globals.workspace.pinnedTracks.map((track) => {
            const tr = globals.trackManager.getTrackRenderer(track.uri);
            return new TrackPanel({
              track: track,
              title: track.displayName,
              tags: tr?.desc.tags,
              trackRenderer: tr,
              revealOnCreate: true,
              chips: tr?.desc.chips,
              pluginId: tr?.desc.pluginId,
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
              renderOverlay,
            }),
      ),
      m(TabPanel),
    );

    globals.trackManager.flushOldTracks();
    return result;
  }
}

function renderOverlay(
  ctx: CanvasRenderingContext2D,
  canvasSize: Size2D,
  panels: ReadonlyArray<RenderedPanelInfo>,
): void {
  const size = {
    width: canvasSize.width - TRACK_SHELL_WIDTH,
    height: canvasSize.height,
  };

  using _ = canvasSave(ctx);
  ctx.translate(TRACK_SHELL_WIDTH, 0);
  canvasClip(ctx, 0, 0, size.width, size.height);
  renderFlows(ctx, size, panels);
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
function renderToplevelPanels(filterTerm: Optional<string>): PanelOrGroup[] {
  return renderNodes(globals.workspace.children, filterTerm);
}

// Given a list of tracks and a filter term, return a list pf panels filtered by
// the filter term
function renderNodes(
  nodes: ReadonlyArray<Node>,
  filterTerm?: string,
): PanelOrGroup[] {
  return nodes.flatMap((node) => {
    if (node instanceof GroupNode) {
      if (node.headless) {
        return renderNodes(node.children, filterTerm);
      } else {
        if (filterTermIsValid(filterTerm)) {
          const tokens = tokenizeFilterTerm(filterTerm);
          const match = fuzzyMatch(node.displayName, ...tokens);
          if (match.matches) {
            return {
              kind: 'group',
              collapsed: node.collapsed,
              header: renderGroupHeaderPanel(node, true, node.collapsed),
              childPanels: node.collapsed ? [] : renderNodes(node.children),
            };
          } else {
            const childPanels = renderNodes(node.children, filterTerm);
            if (childPanels.length > 0) {
              return {
                kind: 'group',
                collapsed: false,
                header: renderGroupHeaderPanel(node, false, node.collapsed),
                childPanels,
              };
            }
            return [];
          }
        } else {
          return {
            kind: 'group',
            collapsed: node.collapsed,
            header: renderGroupHeaderPanel(node, true, node.collapsed),
            childPanels: node.collapsed
              ? []
              : renderNodes(node.children, filterTerm),
          };
        }
      }
    } else {
      if (filterTermIsValid(filterTerm)) {
        const tokens = tokenizeFilterTerm(filterTerm);
        const match = fuzzyMatch(node.displayName, ...tokens);
        if (match.matches) {
          return renderTrackPanel(node);
        } else {
          return [];
        }
      } else {
        return renderTrackPanel(node);
      }
    }
  });
}

function renderTrackPanel(track: TrackNode) {
  const tr = globals.trackManager.getTrackRenderer(track.uri);
  return new TrackPanel({
    track: track,
    title: track.displayName,
    tags: tr?.desc.tags,
    trackRenderer: tr,
    chips: tr?.desc.chips,
    pluginId: tr?.desc.pluginId,
  });
}

function renderGroupHeaderPanel(
  group: GroupNode,
  collapsable: boolean,
  collapsed: boolean,
): TrackGroupPanel {
  if (group.headerTrackUri !== undefined) {
    const tr = globals.trackManager.getTrackRenderer(group.headerTrackUri);
    return new TrackGroupPanel({
      groupNode: group,
      trackRenderer: tr,
      subtitle: tr?.desc.subtitle,
      tags: tr?.desc.tags,
      chips: tr?.desc.chips,
      collapsed,
      title: group.displayName,
      collapsable,
    });
  } else {
    return new TrackGroupPanel({
      groupNode: group,
      collapsed,
      title: group.displayName,
      collapsable,
    });
  }
}

export const ViewerPage = createPage({
  view() {
    return m(TraceViewer);
  },
});
