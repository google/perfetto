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

import {hex} from 'color-convert';
import m from 'mithril';
import {removeFalsyValues} from '../base/array_utils';
import {canvasClip, canvasSave} from '../base/canvas_utils';
import {findRef, toHTMLElement} from '../base/dom_utils';
import {Size2D, VerticalBounds} from '../base/geom';
import {assertExists} from '../base/logging';
import {clamp} from '../base/math_utils';
import {Time, TimeSpan} from '../base/time';
import {TimeScale} from '../base/time_scale';
import {featureFlags} from '../core/feature_flags';
import {raf} from '../core/raf_scheduler';
import {TrackNode} from '../public/workspace';
import {TRACK_BORDER_COLOR, TRACK_SHELL_WIDTH} from './css_constants';
import {renderFlows} from './flow_events_renderer';
import {generateTicks, getMaxMajorTicks, TickType} from './gridline_helper';
import {NotesPanel} from './notes_panel';
import {OverviewTimelinePanel} from './overview_timeline_panel';
import {PanAndZoomHandler} from './pan_and_zoom_handler';
import {
  PanelContainer,
  PanelOrGroup,
  RenderedPanelInfo,
} from './panel_container';
import {TabPanel} from './tab_panel';
import {TickmarkPanel} from './tickmark_panel';
import {TimeAxisPanel} from './time_axis_panel';
import {TimeSelectionPanel} from './time_selection_panel';
import {TrackPanel} from './track_panel';
import {drawVerticalLineAtTime} from './vertical_line_helper';
import {TraceImpl} from '../core/trace_impl';
import {PageWithTraceImplAttrs} from '../core/page_manager';
import {AppImpl} from '../core/app_impl';

const OVERVIEW_PANEL_FLAG = featureFlags.register({
  id: 'overviewVisible',
  name: 'Overview Panel',
  description: 'Show the panel providing an overview of the trace',
  defaultValue: true,
});

// Checks if the mousePos is within 3px of the start or end of the
// current selected time range.
function onTimeRangeBoundary(
  trace: TraceImpl,
  timescale: TimeScale,
  mousePos: number,
): 'START' | 'END' | null {
  const selection = trace.selection.selection;
  if (selection.kind === 'area') {
    // If frontend selectedArea exists then we are in the process of editing the
    // time range and need to use that value instead.
    const area = trace.timeline.selectedArea
      ? trace.timeline.selectedArea
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

interface SelectedContainer {
  readonly containerClass: string;
  readonly dragStartAbsY: number;
  readonly dragEndAbsY: number;
}

/**
 * Top-most level component for the viewer page. Holds tracks, brush timeline,
 * panels, and everything else that's part of the main trace viewer page.
 */
export class ViewerPage implements m.ClassComponent<PageWithTraceImplAttrs> {
  private zoomContent?: PanAndZoomHandler;
  // Used to prevent global deselection if a pan/drag select occurred.
  private keepCurrentSelection = false;

  private overviewTimelinePanel: OverviewTimelinePanel;
  private timeAxisPanel: TimeAxisPanel;
  private timeSelectionPanel: TimeSelectionPanel;
  private notesPanel: NotesPanel;
  private tickmarkPanel: TickmarkPanel;
  private timelineWidthPx?: number;
  private selectedContainer?: SelectedContainer;
  private showPanningHint = false;

  private readonly PAN_ZOOM_CONTENT_REF = 'pan-and-zoom-content';

  constructor(vnode: m.CVnode<PageWithTraceImplAttrs>) {
    this.notesPanel = new NotesPanel(vnode.attrs.trace);
    this.timeAxisPanel = new TimeAxisPanel(vnode.attrs.trace);
    this.timeSelectionPanel = new TimeSelectionPanel(vnode.attrs.trace);
    this.tickmarkPanel = new TickmarkPanel(vnode.attrs.trace);
    this.overviewTimelinePanel = new OverviewTimelinePanel(vnode.attrs.trace);
    this.notesPanel = new NotesPanel(vnode.attrs.trace);
    this.timeSelectionPanel = new TimeSelectionPanel(vnode.attrs.trace);
  }

  oncreate({dom, attrs}: m.CVnodeDOM<PageWithTraceImplAttrs>) {
    const panZoomElRaw = findRef(dom, this.PAN_ZOOM_CONTENT_REF);
    const panZoomEl = toHTMLElement(assertExists(panZoomElRaw));

    const {top: panTop} = panZoomEl.getBoundingClientRect();
    this.zoomContent = new PanAndZoomHandler({
      element: panZoomEl,
      onPanned: (pannedPx: number) => {
        const timeline = attrs.trace.timeline;

        if (this.timelineWidthPx === undefined) return;

        this.keepCurrentSelection = true;
        const timescale = new TimeScale(timeline.visibleWindow, {
          left: 0,
          right: this.timelineWidthPx,
        });
        const tDelta = timescale.pxToDuration(pannedPx);
        timeline.panVisibleWindow(tDelta);
      },
      onZoomed: (zoomedPositionPx: number, zoomRatio: number) => {
        const timeline = attrs.trace.timeline;
        // TODO(hjd): Avoid hardcoding TRACK_SHELL_WIDTH.
        // TODO(hjd): Improve support for zooming in overview timeline.
        const zoomPx = zoomedPositionPx - TRACK_SHELL_WIDTH;
        const rect = dom.getBoundingClientRect();
        const centerPoint = zoomPx / (rect.width - TRACK_SHELL_WIDTH);
        timeline.zoomVisibleWindow(1 - zoomRatio, centerPoint);
        raf.scheduleRedraw();
      },
      editSelection: (currentPx: number) => {
        if (this.timelineWidthPx === undefined) return false;
        const timescale = new TimeScale(attrs.trace.timeline.visibleWindow, {
          left: 0,
          right: this.timelineWidthPx,
        });
        return onTimeRangeBoundary(attrs.trace, timescale, currentPx) !== null;
      },
      onSelection: (
        dragStartX: number,
        dragStartY: number,
        prevX: number,
        currentX: number,
        currentY: number,
        editing: boolean,
      ) => {
        const traceTime = attrs.trace.traceInfo;
        const timeline = attrs.trace.timeline;

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
          const selection = attrs.trace.selection.selection;
          if (selection.kind === 'area') {
            const area = attrs.trace.timeline.selectedArea
              ? attrs.trace.timeline.selectedArea
              : selection;
            let newTime = timescale
              .pxToHpTime(currentX - TRACK_SHELL_WIDTH)
              .toTime();
            // Have to check again for when one boundary crosses over the other.
            const curBoundary = onTimeRangeBoundary(
              attrs.trace,
              timescale,
              prevX,
            );
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

          const absStartY = dragStartY + panTop;
          const absCurrentY = currentY + panTop;
          if (this.selectedContainer === undefined) {
            for (const c of dom.querySelectorAll('.pf-panel-container')) {
              const {top, bottom} = c.getBoundingClientRect();
              if (top <= absStartY && absCurrentY <= bottom) {
                const stack = assertExists(c.querySelector('.pf-panel-stack'));
                const stackTop = stack.getBoundingClientRect().top;
                this.selectedContainer = {
                  containerClass: Array.from(c.classList).filter(
                    (x) => x !== 'pf-panel-container',
                  )[0],
                  dragStartAbsY: -stackTop + absStartY,
                  dragEndAbsY: -stackTop + absCurrentY,
                };
                break;
              }
            }
          } else {
            const c = assertExists(
              dom.querySelector(`.${this.selectedContainer.containerClass}`),
            );
            const {top, bottom} = c.getBoundingClientRect();
            const boundedCurrentY = Math.min(
              Math.max(top, absCurrentY),
              bottom,
            );
            const stack = assertExists(c.querySelector('.pf-panel-stack'));
            const stackTop = stack.getBoundingClientRect().top;
            this.selectedContainer = {
              ...this.selectedContainer,
              dragEndAbsY: -stackTop + boundedCurrentY,
            };
          }
          this.showPanningHint = true;
        }
        raf.scheduleRedraw();
      },
      endSelection: (edit: boolean) => {
        this.selectedContainer = undefined;
        const area = attrs.trace.timeline.selectedArea;
        // If we are editing we need to pass the current id through to ensure
        // the marked area with that id is also updated.
        if (edit) {
          const selection = attrs.trace.selection.selection;
          if (selection.kind === 'area' && area) {
            attrs.trace.selection.selectArea({...area});
          }
        } else if (area) {
          attrs.trace.selection.selectArea({...area});
        }
        // Now the selection has ended we stored the final selected area in the
        // global state and can remove the in progress selection from the
        // timeline.
        attrs.trace.timeline.deselectArea();
        // Full redraw to color track shell.
        raf.scheduleFullRedraw();
      },
    });
  }

  onremove() {
    if (this.zoomContent) this.zoomContent[Symbol.dispose]();
  }

  view({attrs}: m.CVnode<PageWithTraceImplAttrs>) {
    const scrollingPanels = renderToplevelPanels(attrs.trace);

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
            attrs.trace.selection.clear();
          },
        },
        m(
          '.pf-timeline-header',
          m(PanelContainer, {
            trace: attrs.trace,
            className: 'header-panel-container',
            panels: removeFalsyValues([
              OVERVIEW_PANEL_FLAG.get() && this.overviewTimelinePanel,
              this.timeAxisPanel,
              this.timeSelectionPanel,
              this.notesPanel,
              this.tickmarkPanel,
            ]),
            selectedYRange: this.getYRange('header-panel-container'),
          }),
          m('.scrollbar-spacer-vertical'),
        ),
        m(PanelContainer, {
          trace: attrs.trace,
          className: 'pinned-panel-container',
          panels: attrs.trace.workspace.pinnedTracks.map((trackNode) => {
            if (trackNode.uri) {
              const tr = attrs.trace.tracks.getTrackRenderer(trackNode.uri);
              return new TrackPanel({
                trace: attrs.trace,
                reorderable: true,
                node: trackNode,
                trackRenderer: tr,
                revealOnCreate: true,
                indentationLevel: 0,
                topOffsetPx: 0,
              });
            } else {
              return new TrackPanel({
                trace: attrs.trace,
                node: trackNode,
                revealOnCreate: true,
                indentationLevel: 0,
                topOffsetPx: 0,
              });
            }
          }),
          renderUnderlay: (ctx, size) => renderUnderlay(attrs.trace, ctx, size),
          renderOverlay: (ctx, size, panels) =>
            renderOverlay(attrs.trace, ctx, size, panels),
          selectedYRange: this.getYRange('pinned-panel-container'),
        }),
        m(PanelContainer, {
          trace: attrs.trace,
          className: 'scrolling-panel-container',
          panels: scrollingPanels,
          onPanelStackResize: (width) => {
            const timelineWidth = width - TRACK_SHELL_WIDTH;
            this.timelineWidthPx = timelineWidth;
          },
          renderUnderlay: (ctx, size) => renderUnderlay(attrs.trace, ctx, size),
          renderOverlay: (ctx, size, panels) =>
            renderOverlay(attrs.trace, ctx, size, panels),
          selectedYRange: this.getYRange('scrolling-panel-container'),
        }),
      ),
      m(TabPanel, {
        trace: attrs.trace,
      }),
      this.showPanningHint && m(HelpPanningNotification),
    );

    attrs.trace.tracks.flushOldTracks();
    return result;
  }

  private getYRange(cls: string): VerticalBounds | undefined {
    if (this.selectedContainer?.containerClass !== cls) {
      return undefined;
    }
    const {dragStartAbsY, dragEndAbsY} = this.selectedContainer;
    return {
      top: Math.min(dragStartAbsY, dragEndAbsY),
      bottom: Math.max(dragStartAbsY, dragEndAbsY),
    };
  }
}

function renderUnderlay(
  trace: TraceImpl,
  ctx: CanvasRenderingContext2D,
  canvasSize: Size2D,
): void {
  const size = {
    width: canvasSize.width - TRACK_SHELL_WIDTH,
    height: canvasSize.height,
  };

  using _ = canvasSave(ctx);
  ctx.translate(TRACK_SHELL_WIDTH, 0);

  const timewindow = trace.timeline.visibleWindow;
  const timescale = new TimeScale(timewindow, {left: 0, right: size.width});

  // Just render the gridlines - these should appear underneath all tracks
  drawGridLines(trace, ctx, timewindow.toTimeSpan(), timescale, size);
}

function renderOverlay(
  trace: TraceImpl,
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

  // TODO(primiano): plumb the TraceImpl obj throughout the viwer page.
  renderFlows(trace, ctx, size, panels);

  const timewindow = trace.timeline.visibleWindow;
  const timescale = new TimeScale(timewindow, {left: 0, right: size.width});

  renderHoveredNoteVertical(trace, ctx, timescale, size);
  renderHoveredCursorVertical(trace, ctx, timescale, size);
  renderWakeupVertical(trace, ctx, timescale, size);
  renderNoteVerticals(trace, ctx, timescale, size);
}

// Render the toplevel "scrolling" tracks and track groups
function renderToplevelPanels(trace: TraceImpl): PanelOrGroup[] {
  return renderNodes(trace, trace.workspace.children, 0, 0);
}

// Given a list of tracks and a filter term, return a list pf panels filtered by
// the filter term
function renderNodes(
  trace: TraceImpl,
  nodes: ReadonlyArray<TrackNode>,
  indent: number,
  topOffsetPx: number,
): PanelOrGroup[] {
  return nodes.flatMap((node) => {
    if (node.headless) {
      // Render children as if this node doesn't exist
      return renderNodes(trace, node.children, indent, topOffsetPx);
    } else if (node.children.length === 0) {
      return renderTrackPanel(trace, node, indent, topOffsetPx);
    } else {
      const headerPanel = renderTrackPanel(trace, node, indent, topOffsetPx);
      const isSticky = node.isSummary;
      const nextTopOffsetPx = isSticky
        ? topOffsetPx + headerPanel.heightPx
        : topOffsetPx;
      return {
        kind: 'group',
        collapsed: node.collapsed,
        header: headerPanel,
        sticky: isSticky, // && node.collapsed??
        topOffsetPx,
        childPanels: node.collapsed
          ? []
          : renderNodes(trace, node.children, indent + 1, nextTopOffsetPx),
      };
    }
  });
}

function renderTrackPanel(
  trace: TraceImpl,
  trackNode: TrackNode,
  indent: number,
  topOffsetPx: number,
) {
  let tr = undefined;
  if (trackNode.uri) {
    tr = trace.tracks.getTrackRenderer(trackNode.uri);
  }
  return new TrackPanel({
    trace,
    node: trackNode,
    trackRenderer: tr,
    indentationLevel: indent,
    topOffsetPx,
  });
}

export function drawGridLines(
  trace: TraceImpl,
  ctx: CanvasRenderingContext2D,
  timespan: TimeSpan,
  timescale: TimeScale,
  size: Size2D,
): void {
  ctx.strokeStyle = TRACK_BORDER_COLOR;
  ctx.lineWidth = 1;

  if (size.width > 0 && timespan.duration > 0n) {
    const maxMajorTicks = getMaxMajorTicks(size.width);
    const offset = trace.timeline.timestampOffset();
    for (const {type, time} of generateTicks(timespan, maxMajorTicks, offset)) {
      const px = Math.floor(timescale.timeToPx(time));
      if (type === TickType.MAJOR) {
        ctx.beginPath();
        ctx.moveTo(px + 0.5, 0);
        ctx.lineTo(px + 0.5, size.height);
        ctx.stroke();
      }
    }
  }
}

export function renderHoveredCursorVertical(
  trace: TraceImpl,
  ctx: CanvasRenderingContext2D,
  timescale: TimeScale,
  size: Size2D,
) {
  if (trace.timeline.hoverCursorTimestamp !== undefined) {
    drawVerticalLineAtTime(
      ctx,
      timescale,
      trace.timeline.hoverCursorTimestamp,
      size.height,
      `#344596`,
    );
  }
}

export function renderHoveredNoteVertical(
  trace: TraceImpl,
  ctx: CanvasRenderingContext2D,
  timescale: TimeScale,
  size: Size2D,
) {
  if (trace.timeline.hoveredNoteTimestamp !== undefined) {
    drawVerticalLineAtTime(
      ctx,
      timescale,
      trace.timeline.hoveredNoteTimestamp,
      size.height,
      `#aaa`,
    );
  }
}

export function renderWakeupVertical(
  trace: TraceImpl,
  ctx: CanvasRenderingContext2D,
  timescale: TimeScale,
  size: Size2D,
) {
  const selection = trace.selection.selection;
  if (selection.kind === 'track_event' && selection.wakeupTs) {
    drawVerticalLineAtTime(
      ctx,
      timescale,
      selection.wakeupTs,
      size.height,
      `black`,
    );
  }
}

export function renderNoteVerticals(
  trace: TraceImpl,
  ctx: CanvasRenderingContext2D,
  timescale: TimeScale,
  size: Size2D,
) {
  // All marked areas should have semi-transparent vertical lines
  // marking the start and end.
  for (const note of trace.notes.notes.values()) {
    if (note.noteType === 'SPAN') {
      const transparentNoteColor =
        'rgba(' + hex.rgb(note.color.substr(1)).toString() + ', 0.65)';
      drawVerticalLineAtTime(
        ctx,
        timescale,
        note.start,
        size.height,
        transparentNoteColor,
        1,
      );
      drawVerticalLineAtTime(
        ctx,
        timescale,
        note.end,
        size.height,
        transparentNoteColor,
        1,
      );
    } else if (note.noteType === 'DEFAULT') {
      drawVerticalLineAtTime(
        ctx,
        timescale,
        note.timestamp,
        size.height,
        note.color,
      );
    }
  }
}

class HelpPanningNotification implements m.ClassComponent {
  private readonly PANNING_HINT_KEY = 'dismissedPanningHint';
  private dismissed = localStorage.getItem(this.PANNING_HINT_KEY) === 'true';

  view() {
    // Do not show the help notification in embedded mode because local storage
    // does not persist for iFrames. The host is responsible for communicating
    // to users that they can press '?' for help.
    if (AppImpl.instance.embeddedMode || this.dismissed) {
      return;
    }
    return m(
      '.helpful-hint',
      m(
        '.hint-text',
        'Are you trying to pan? Use the WASD keys or hold shift to click ' +
          "and drag. Press '?' for more help.",
      ),
      m(
        'button.hint-dismiss-button',
        {
          onclick: () => {
            this.dismissed = true;
            localStorage.setItem(this.PANNING_HINT_KEY, 'true');
            raf.scheduleFullRedraw();
          },
        },
        'Dismiss',
      ),
    );
  }
}
