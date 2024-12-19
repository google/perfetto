// Copyright (C) 2024 The Android Open Source Project
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

/**
 * This module provides the TrackNodeTree mithril component, which is
 * responsible for rendering out a tree of tracks and drawing their content
 * onto the canvas.
 * - Rendering track panels and handling nested and sticky headers.
 * - Managing the virtual canvas & drawing the grid-lines, tracks and overlays
 *   onto the canvas.
 * - Handling track interaction events such as dragging, panning and scrolling.
 */

import {hex} from 'color-convert';
import m from 'mithril';
import {canvasClip, canvasSave} from '../../base/canvas_utils';
import {classNames} from '../../base/classnames';
import {DisposableStack} from '../../base/disposable_stack';
import {findRef, toHTMLElement} from '../../base/dom_utils';
import {Rect2D, Size2D, VerticalBounds} from '../../base/geom';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {assertExists} from '../../base/logging';
import {Time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {ZonedInteractionHandler} from '../../base/zoned_interaction_handler';
import {PerfStats, runningStatStr} from '../../core/perf_stats';
import {raf} from '../../core/raf_scheduler';
import {TraceImpl} from '../../core/trace_impl';
import {TrackNode} from '../../public/workspace';
import {VirtualOverlayCanvas} from '../../widgets/virtual_overlay_canvas';
import {
  SELECTION_STROKE_COLOR,
  TRACK_BORDER_COLOR,
  TRACK_SHELL_WIDTH,
} from '../css_constants';
import {renderFlows} from './flow_events_renderer';
import {generateTicks, getMaxMajorTicks, TickType} from './gridline_helper';
import {
  shiftDragPanInteraction,
  wheelNavigationInteraction,
} from './timeline_interactions';
import {TrackView} from './track_view';
import {drawVerticalLineAtTime} from './vertical_line_helper';

export interface TrackTreeViewAttrs {
  // Access to the trace, for accessing the track registry / selection manager.
  readonly trace: TraceImpl;

  // The root track node for tracks to display in this stack. This node is not
  // actually displayed, only its children are, but it's used for reordering
  // purposes if `reorderable` is set to true.
  readonly rootNode: TrackNode;

  // Additional class names to add to the root level element.
  readonly className?: string;

  // Whether tracks in this stack can be reordered amongst themselves.
  // Default: false
  readonly reorderable?: boolean;

  // Scroll to scroll to new tracks as they are added.
  // Default: false
  readonly scrollToNewTracks?: boolean;
}

const TRACK_CONTAINER_REF = 'track-container';

export class TrackTreeView implements m.ClassComponent<TrackTreeViewAttrs> {
  private readonly trace: TraceImpl;
  private readonly trash = new DisposableStack();
  private currentSelectionRect?: Rect2D;
  private interactions?: ZonedInteractionHandler;
  private perfStatsEnabled = false;
  private trackPerfStats = new WeakMap<TrackNode, PerfStats>();
  private perfStats = {
    totalTracks: 0,
    tracksOnCanvas: 0,
    renderStats: new PerfStats(10),
  };

  constructor({attrs}: m.Vnode<TrackTreeViewAttrs>) {
    this.trace = attrs.trace;
  }

  view({attrs}: m.Vnode<TrackTreeViewAttrs>): m.Children {
    const {trace, scrollToNewTracks, reorderable, className, rootNode} = attrs;
    const renderedTracks = new Array<TrackView>();
    let top = 0;

    function renderTrack(
      node: TrackNode,
      depth = 0,
      stickyTop = 0,
    ): m.Children {
      const trackView = new TrackView(trace, node, top);
      renderedTracks.push(trackView);

      let childDepth = depth;
      let childStickyTop = stickyTop;
      if (!node.headless) {
        top += trackView.height;
        ++childDepth;
        childStickyTop += trackView.height;
      }

      const children =
        (node.headless || node.expanded) &&
        node.hasChildren &&
        node.children.map((track) =>
          renderTrack(track, childDepth, childStickyTop),
        );

      if (node.headless) {
        return children;
      } else {
        return trackView.renderDOM(
          {
            scrollToOnCreate: scrollToNewTracks,
            reorderable,
            stickyTop,
            depth,
          },
          children,
        );
      }
    }

    return m(
      VirtualOverlayCanvas,
      {
        className: classNames(className, 'pf-track-tree'),
        scrollAxes: 'y',
        onCanvasRedraw: ({ctx, virtualCanvasSize, canvasRect}) => {
          this.drawCanvas(
            ctx,
            virtualCanvasSize,
            renderedTracks,
            canvasRect,
            rootNode,
          );
        },
        onCanvasCreate: (overlay) => {
          overlay.trash.use(
            raf.addCanvasRedrawCallback(() => overlay.redrawCanvas()),
          );
        },
      },
      m(
        '',
        {ref: TRACK_CONTAINER_REF},
        rootNode.children.map((track) => renderTrack(track)),
      ),
    );
  }

  oncreate({attrs, dom}: m.VnodeDOM<TrackTreeViewAttrs>) {
    const interactionTarget = toHTMLElement(
      assertExists(findRef(dom, TRACK_CONTAINER_REF)),
    );
    this.interactions = new ZonedInteractionHandler(interactionTarget);
    this.trash.use(this.interactions);
    this.trash.use(
      attrs.trace.perfDebugging.addContainer({
        setPerfStatsEnabled: (enable: boolean) => {
          this.perfStatsEnabled = enable;
        },
        renderPerfStats: () => {
          return [
            m(
              '',
              `${this.perfStats.totalTracks} tracks, ` +
                `${this.perfStats.tracksOnCanvas} on canvas.`,
            ),
            m('', runningStatStr(this.perfStats.renderStats)),
          ];
        },
      }),
    );
  }

  onremove() {
    this.trash.dispose();
  }

  private drawCanvas(
    ctx: CanvasRenderingContext2D,
    size: Size2D,
    renderedTracks: ReadonlyArray<TrackView>,
    floatingCanvasRect: Rect2D,
    rootNode: TrackNode,
  ) {
    const timelineRect = new Rect2D({
      left: TRACK_SHELL_WIDTH,
      top: 0,
      right: size.width,
      bottom: size.height,
    });

    // Always grab the latest visible window and create a timescale out of
    // it.
    const visibleWindow = this.trace.timeline.visibleWindow;
    const timescale = new TimeScale(visibleWindow, timelineRect);

    const start = performance.now();

    // Save, translate & clip the canvas to the area of the timeline.
    using _ = canvasSave(ctx);
    canvasClip(ctx, timelineRect);

    this.drawGridLines(ctx, timescale, timelineRect);

    const tracksOnCanvas = this.drawTracks(
      renderedTracks,
      floatingCanvasRect,
      size,
      ctx,
      timelineRect,
      visibleWindow,
    );

    renderFlows(this.trace, ctx, size, renderedTracks, rootNode, timescale);
    this.drawHoveredNoteVertical(ctx, timescale, size);
    this.drawHoveredCursorVertical(ctx, timescale, size);
    this.drawWakeupVertical(ctx, timescale, size);
    this.drawNoteVerticals(ctx, timescale, size);
    this.drawTemporarySelectionRect(ctx);
    this.updateInteractions(timelineRect, timescale, size, renderedTracks);

    const renderTime = performance.now() - start;
    this.updatePerfStats(renderTime, renderedTracks.length, tracksOnCanvas);
  }

  private drawGridLines(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
  ): void {
    ctx.strokeStyle = TRACK_BORDER_COLOR;
    ctx.lineWidth = 1;

    if (size.width > 0 && timescale.timeSpan.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width);
      const offset = this.trace.timeline.timestampOffset();
      for (const {type, time} of generateTicks(
        timescale.timeSpan.toTimeSpan(),
        maxMajorTicks,
        offset,
      )) {
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

  private drawTracks(
    renderedTracks: ReadonlyArray<TrackView>,
    floatingCanvasRect: Rect2D,
    size: Size2D,
    ctx: CanvasRenderingContext2D,
    timelineRect: Rect2D,
    visibleWindow: HighPrecisionTimeSpan,
  ) {
    let tracksOnCanvas = 0;
    for (const trackView of renderedTracks) {
      const {verticalBounds} = trackView;
      if (
        floatingCanvasRect.overlaps({
          ...verticalBounds,
          left: 0,
          right: size.width,
        })
      ) {
        trackView.drawCanvas(
          ctx,
          timelineRect,
          visibleWindow,
          this.perfStatsEnabled,
          this.trackPerfStats,
        );
        ++tracksOnCanvas;
      }
    }
    return tracksOnCanvas;
  }

  private updateInteractions(
    timelineRect: Rect2D,
    timescale: TimeScale,
    size: Size2D,
    renderedTracks: ReadonlyArray<TrackView>,
  ) {
    const trace = this.trace;
    const areaSelection =
      trace.selection.selection.kind === 'area' && trace.selection.selection;

    assertExists(this.interactions).update([
      shiftDragPanInteraction(trace, timelineRect, timescale),
      areaSelection !== false && {
        id: 'start-edit',
        area: new Rect2D({
          left: timescale.timeToPx(areaSelection.start) - 5,
          right: timescale.timeToPx(areaSelection.start) + 5,
          top: 0,
          bottom: size.height,
        }),
        cursor: 'col-resize',
        drag: {
          onDragEnd: (e) => {
            const newStartTime = timescale
              .pxToHpTime(e.dragCurrent.x)
              .toTime('ceil');
            trace.selection.selectArea({
              ...areaSelection,
              end: Time.max(newStartTime, areaSelection.end),
              start: Time.min(newStartTime, areaSelection.end),
            });
          },
        },
      },
      areaSelection !== false && {
        id: 'end-edit',
        area: new Rect2D({
          left: timescale.timeToPx(areaSelection.end) - 5,
          right: timescale.timeToPx(areaSelection.end) + 5,
          top: 0,
          bottom: size.height,
        }),
        cursor: 'col-resize',
        drag: {
          onDragEnd: (e) => {
            const newEndTime = timescale
              .pxToHpTime(e.dragCurrent.x)
              .toTime('ceil');
            trace.selection.selectArea({
              ...areaSelection,
              end: Time.max(newEndTime, areaSelection.start),
              start: Time.min(newEndTime, areaSelection.start),
            });
          },
        },
      },
      {
        id: 'area-selection',
        area: timelineRect,
        onClick: () => {
          // If a track hasn't intercepted the click, treat this as a
          // deselection event.
          trace.selection.clear();
        },
        drag: {
          minDistance: 1,
          cursorWhileDragging: 'crosshair',
          onDrag: (e) => {
            const dragRect = Rect2D.fromPoints(e.dragStart, e.dragCurrent);
            const timeSpan = timescale
              .pxSpanToHpTimeSpan(dragRect)
              .toTimeSpan();
            trace.timeline.selectedSpan = timeSpan;
            this.currentSelectionRect = dragRect;
          },
          onDragEnd: (e) => {
            const dragRect = Rect2D.fromPoints(e.dragStart, e.dragCurrent);
            const timeSpan = timescale
              .pxSpanToHpTimeSpan(dragRect)
              .toTimeSpan();
            // Find the list of tracks that intersect this selection
            const trackUris = findTracksInRect(renderedTracks, dragRect, true)
              .map((t) => t.uri)
              .filter((uri) => uri !== undefined);
            trace.selection.selectArea({
              start: timeSpan.start,
              end: timeSpan.end,
              trackUris,
            });
            trace.timeline.selectedSpan = undefined;
            this.currentSelectionRect = undefined;
          },
        },
      },
      wheelNavigationInteraction(trace, timelineRect, timescale),
    ]);
  }

  private updatePerfStats(
    renderTime: number,
    totalTracks: number,
    tracksOnCanvas: number,
  ) {
    if (!this.perfStatsEnabled) return;
    this.perfStats.renderStats.addValue(renderTime);
    this.perfStats.totalTracks = totalTracks;
    this.perfStats.tracksOnCanvas = tracksOnCanvas;
  }

  private drawTemporarySelectionRect(ctx: CanvasRenderingContext2D) {
    if (this.currentSelectionRect) {
      ctx.strokeStyle = SELECTION_STROKE_COLOR;
      ctx.lineWidth = 1;
      const rect = this.currentSelectionRect;
      ctx.strokeRect(rect.x, rect.y, rect.width, rect.height);
    }
  }

  private drawHoveredCursorVertical(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
  ) {
    if (this.trace.timeline.hoverCursorTimestamp !== undefined) {
      drawVerticalLineAtTime(
        ctx,
        timescale,
        this.trace.timeline.hoverCursorTimestamp,
        size.height,
        `#344596`,
      );
    }
  }

  private drawHoveredNoteVertical(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
  ) {
    if (this.trace.timeline.hoveredNoteTimestamp !== undefined) {
      drawVerticalLineAtTime(
        ctx,
        timescale,
        this.trace.timeline.hoveredNoteTimestamp,
        size.height,
        `#aaa`,
      );
    }
  }

  private drawWakeupVertical(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
  ) {
    const selection = this.trace.selection.selection;
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

  private drawNoteVerticals(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
  ) {
    // All marked areas should have semi-transparent vertical lines
    // marking the start and end.
    for (const note of this.trace.notes.notes.values()) {
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
}

/**
 * Returns a list of track nodes that are contained within a given set of
 * vertical bounds.
 *
 * @param renderedTracks - The list of tracks and their positions.
 * @param bounds - The bounds in which to check.
 * @returns - A list of tracks.
 */
function findTracksInRect(
  renderedTracks: ReadonlyArray<TrackView>,
  bounds: VerticalBounds,
  recurseCollapsedSummaryTracks = false,
): TrackNode[] {
  const tracks: TrackNode[] = [];
  for (const {node, verticalBounds} of renderedTracks) {
    const trackRect = new Rect2D({...verticalBounds, left: 0, right: 1});
    if (trackRect.overlaps({...bounds, left: 0, right: 1})) {
      // Recurse all child tracks if group node is collapsed and is a summary
      if (recurseCollapsedSummaryTracks && node.isSummary && node.collapsed) {
        for (const childTrack of node.flatTracks) {
          tracks.push(childTrack);
        }
      } else {
        tracks.push(node);
      }
    }
  }
  return tracks;
}
