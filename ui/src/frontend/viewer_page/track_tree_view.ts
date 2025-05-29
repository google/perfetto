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
import {
  HorizontalBounds,
  Rect2D,
  Size2D,
  VerticalBounds,
} from '../../base/geom';
import {HighPrecisionTime} from '../../base/high_precision_time';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {assertExists} from '../../base/logging';
import {Time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {
  DragEvent,
  ZonedInteractionHandler,
} from '../../base/zoned_interaction_handler';
import {PerfStats, runningStatStr} from '../../core/perf_stats';
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
import {drawVerticalLineAtTime} from '../../base/vertical_line_helper';
import {featureFlags} from '../../core/feature_flags';
import {EmptyState} from '../../widgets/empty_state';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {CursorTooltip} from '../../widgets/cursor_tooltip';

const VIRTUAL_TRACK_SCROLLING = featureFlags.register({
  id: 'virtualTrackScrolling',
  name: 'Virtual track scrolling',
  description: `[Experimental] Use virtual scrolling in the timeline view to
    improve performance on large traces.`,
  defaultValue: false,
});

export interface TrackTreeViewAttrs {
  // Access to the trace, for accessing the track registry / selection manager.
  readonly trace: TraceImpl;

  // The root track node for tracks to display in this stack. This node is not
  // actually displayed, only its children are, but it's used for reordering
  // purposes if `reorderable` is set to true.
  readonly rootNode: TrackNode;

  // Additional class names to add to the root level element.
  readonly className?: string;

  // Allow nodes to be reordered by dragging and dropping.
  // Default: false
  readonly canReorderNodes?: boolean;

  // Adds a little remove button to each node.
  // Default: false
  readonly canRemoveNodes?: boolean;

  // Scroll to scroll to new tracks as they are added.
  // Default: false
  readonly scrollToNewTracks?: boolean;

  // If supplied, each track will be run though this filter to work out whether
  // to show it or not.
  readonly trackFilter?: (track: TrackNode) => boolean;

  readonly filtersApplied?: boolean;
}

const TRACK_CONTAINER_REF = 'track-container';

export class TrackTreeView implements m.ClassComponent<TrackTreeViewAttrs> {
  private readonly trace: TraceImpl;
  private readonly trash = new DisposableStack();
  private interactions?: ZonedInteractionHandler;
  private perfStatsEnabled = false;
  private trackPerfStats = new WeakMap<TrackNode, PerfStats>();
  private perfStats = {
    totalTracks: 0,
    tracksOnCanvas: 0,
    renderStats: new PerfStats(10),
  };
  private areaDrag?: InProgressAreaSelection;
  private handleDrag?: InProgressHandleDrag;
  private canvasRect?: Rect2D;

  constructor({attrs}: m.Vnode<TrackTreeViewAttrs>) {
    this.trace = attrs.trace;
  }

  private hoveredTrackNode?: TrackNode;

  view({attrs}: m.Vnode<TrackTreeViewAttrs>): m.Children {
    const {
      trace,
      scrollToNewTracks,
      canReorderNodes,
      canRemoveNodes,
      className,
      rootNode,
      trackFilter,
      filtersApplied,
    } = attrs;
    const renderedTracks = new Array<TrackView>();
    let top = 0;

    function filterMatches(node: TrackNode): boolean {
      if (!trackFilter) return true; // Filter ignored, show all tracks.

      // If this track name matches filter, show it.
      if (trackFilter(node)) return true;

      // Also show if any of our children match.
      if (node.children?.some(filterMatches)) return true;

      return false;
    }

    const renderTrack = (
      node: TrackNode,
      depth = 0,
      stickyTop = 0,
    ): m.Children => {
      // Skip nodes that don't match the filter and have no matching children.
      if (!filterMatches(node)) return undefined;

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
        (node.headless || node.expanded || filtersApplied) &&
        node.hasChildren &&
        node.children.map((track) =>
          renderTrack(track, childDepth, childStickyTop),
        );

      if (node.headless) {
        return children;
      } else {
        const isTrackOnScreen = () => {
          if (VIRTUAL_TRACK_SCROLLING.get()) {
            return this.canvasRect?.overlaps({
              left: 0,
              right: 1,
              ...trackView.verticalBounds,
            });
          } else {
            return true;
          }
        };

        return trackView.renderDOM(
          {
            lite: !Boolean(isTrackOnScreen()),
            scrollToOnCreate: scrollToNewTracks,
            reorderable: canReorderNodes,
            removable: canRemoveNodes,
            stickyTop,
            depth,
            collapsible: !filtersApplied,
            onTrackMouseOver: () => {
              this.hoveredTrackNode = node;
            },
            onTrackMouseOut: () => {
              this.hoveredTrackNode = undefined;
            },
          },
          children,
        );
      }
    };

    const trackVnodes = rootNode.children.map((track) => renderTrack(track));

    // If there are no truthy vnode values, show "empty state" placeholder.
    if (trackVnodes.every((x) => !Boolean(x))) {
      if (filtersApplied) {
        // If we are filtering, show 'no matching tracks' empty state widget.
        return m(
          EmptyState,
          {
            className,
            icon: 'filter_alt_off',
            title: `No tracks match track filter`,
          },
          m(Button, {
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            label: 'Clear track filter',
            onclick: () => trace.tracks.filters.clearAll(),
          }),
        );
      } else {
        // Not filtering, the workspace must be empty.
        return m(EmptyState, {
          className,
          icon: 'inbox',
          title: 'Empty workspace',
        });
      }
    }

    return m(
      VirtualOverlayCanvas,
      {
        onMount: (redrawCanvas) =>
          attrs.trace.raf.addCanvasRedrawCallback(redrawCanvas),
        disableCanvasRedrawOnMithrilUpdates: true,
        className: classNames(className, 'pf-track-tree'),
        overflowY: 'auto',
        overflowX: 'hidden',
        onCanvasRedraw: ({ctx, virtualCanvasSize, canvasRect}) => {
          this.drawCanvas(
            ctx,
            virtualCanvasSize,
            renderedTracks,
            canvasRect,
            rootNode,
          );

          if (VIRTUAL_TRACK_SCROLLING.get()) {
            // The VOC can ask us to redraw the canvas for any number of
            // reasons, we're interested in the case where the canvas rect has
            // moved (which indicates that the user has scrolled enough to
            // warrant drawing more content). If so, we should redraw the DOM in
            // order to keep the track nodes inside the viewport rendering in
            // full-fat mode.
            if (
              this.canvasRect === undefined ||
              !this.canvasRect.equals(canvasRect)
            ) {
              this.canvasRect = canvasRect;
              m.redraw();
            }
          }
        },
      },
      m('', {ref: TRACK_CONTAINER_REF}, trackVnodes),
      this.hoveredTrackNode && this.renderPopup(this.hoveredTrackNode),
    );
  }

  private renderPopup(trackNode: TrackNode) {
    const track = trackNode.uri
      ? this.trace.tracks.getTrack(trackNode.uri)
      : undefined;
    const tooltipNodes = track?.track.renderTooltip?.();
    if (!Boolean(tooltipNodes)) {
      return;
    }
    return m(CursorTooltip, {className: 'pf-track__tooltip'}, tooltipNodes);
  }

  oncreate(vnode: m.VnodeDOM<TrackTreeViewAttrs>) {
    this.trash.use(
      vnode.attrs.trace.perfDebugging.addContainer({
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

    this.onupdate(vnode);
  }

  onupdate({dom}: m.VnodeDOM<TrackTreeViewAttrs>) {
    // Depending on the state of the filter/workspace, we sometimes have a
    // TRACK_CONTAINER_REF element and sometimes we don't (see the view
    // function). This means the DOM element could potentially appear/disappear
    // or change every update cycle. This chunk of code hooks the
    // ZonedInteractionHandler back up again if the DOM element is present,
    // otherwise it just removes it.
    const interactionTarget = findRef(dom, TRACK_CONTAINER_REF) ?? undefined;
    if (interactionTarget !== this.interactions?.target) {
      this.interactions?.[Symbol.dispose]();
      if (!interactionTarget) {
        this.interactions = undefined;
      } else {
        this.interactions = new ZonedInteractionHandler(
          toHTMLElement(interactionTarget),
        );
      }
    }
  }

  onremove() {
    this.interactions?.[Symbol.dispose]();
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
    this.drawNoteVerticals(ctx, timescale, size);
    this.drawAreaSelection(ctx, timescale, size);
    this.updateInteractions(timelineRect, timescale, size, renderedTracks);

    this.trace.tracks.overlays.forEach((overlay) => {
      overlay.render(ctx, timescale, size, renderedTracks);
    });

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
          cursorWhileDragging: 'col-resize',
          onDrag: (e) => {
            if (!this.handleDrag) {
              this.handleDrag = new InProgressHandleDrag(
                new HighPrecisionTime(areaSelection.end),
              );
            }
            this.handleDrag.currentTime = timescale.pxToHpTime(e.dragCurrent.x);
            trace.timeline.selectedSpan = this.handleDrag
              .timeSpan()
              .toTimeSpan();
          },
          onDragEnd: (e) => {
            const newStartTime = timescale
              .pxToHpTime(e.dragCurrent.x)
              .toTime('ceil');
            trace.selection.selectArea({
              ...areaSelection,
              end: Time.max(newStartTime, areaSelection.end),
              start: Time.min(newStartTime, areaSelection.end),
            });
            trace.timeline.selectedSpan = undefined;
            this.handleDrag = undefined;
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
          cursorWhileDragging: 'col-resize',
          onDrag: (e) => {
            if (!this.handleDrag) {
              this.handleDrag = new InProgressHandleDrag(
                new HighPrecisionTime(areaSelection.start),
              );
            }
            this.handleDrag.currentTime = timescale.pxToHpTime(e.dragCurrent.x);
            trace.timeline.selectedSpan = this.handleDrag
              .timeSpan()
              .toTimeSpan();
          },
          onDragEnd: (e) => {
            const newEndTime = timescale
              .pxToHpTime(e.dragCurrent.x)
              .toTime('ceil');
            trace.selection.selectArea({
              ...areaSelection,
              end: Time.max(newEndTime, areaSelection.start),
              start: Time.min(newEndTime, areaSelection.start),
            });
            trace.timeline.selectedSpan = undefined;
            this.handleDrag = undefined;
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
            if (!this.areaDrag) {
              this.areaDrag = new InProgressAreaSelection(
                timescale.pxToHpTime(e.dragStart.x),
                e.dragStart.y,
              );
            }
            this.areaDrag.update(e, timescale);
            trace.timeline.selectedSpan = this.areaDrag.timeSpan().toTimeSpan();
          },
          onDragEnd: (e) => {
            if (!this.areaDrag) {
              this.areaDrag = new InProgressAreaSelection(
                timescale.pxToHpTime(e.dragStart.x),
                e.dragStart.y,
              );
            }
            this.areaDrag?.update(e, timescale);

            // Find the list of tracks that intersect this selection
            const trackUris = findTracksInRect(
              renderedTracks,
              this.areaDrag.rect(timescale),
              true,
            )
              .map((t) => t.uri)
              .filter((uri) => uri !== undefined);

            const timeSpan = this.areaDrag.timeSpan().toTimeSpan();
            trace.selection.selectArea({
              start: timeSpan.start,
              end: timeSpan.end,
              trackUris,
            });

            trace.timeline.selectedSpan = undefined;
            this.areaDrag = undefined;
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

  private drawAreaSelection(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
  ) {
    if (this.areaDrag) {
      ctx.strokeStyle = SELECTION_STROKE_COLOR;
      ctx.lineWidth = 1;
      const rect = this.areaDrag.rect(timescale);
      ctx.strokeRect(rect.x, rect.y, rect.width, rect.height);
    }

    if (this.handleDrag) {
      const rect = this.handleDrag.hBounds(timescale);

      ctx.strokeStyle = SELECTION_STROKE_COLOR;
      ctx.lineWidth = 1;

      ctx.beginPath();
      ctx.moveTo(rect.left, 0);
      ctx.lineTo(rect.left, size.height);
      ctx.stroke();
      ctx.closePath();

      ctx.beginPath();
      ctx.moveTo(rect.right, 0);
      ctx.lineTo(rect.right, size.height);
      ctx.stroke();
      ctx.closePath();
    }

    const selection = this.trace.selection.selection;
    if (selection.kind === 'area') {
      const startPx = timescale.timeToPx(selection.start);
      const endPx = timescale.timeToPx(selection.end);

      ctx.strokeStyle = '#8398e6';
      ctx.lineWidth = 2;

      ctx.beginPath();
      ctx.moveTo(startPx, 0);
      ctx.lineTo(startPx, size.height);
      ctx.stroke();
      ctx.closePath();

      ctx.beginPath();
      ctx.moveTo(endPx, 0);
      ctx.lineTo(endPx, size.height);
      ctx.stroke();
      ctx.closePath();
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

// Stores an in-progress area selection.
class InProgressAreaSelection {
  currentTime: HighPrecisionTime;
  currentY: number;

  constructor(
    readonly startTime: HighPrecisionTime,
    readonly startY: number,
  ) {
    this.currentTime = startTime;
    this.currentY = startY;
  }

  update(e: DragEvent, timescale: TimeScale) {
    this.currentTime = timescale.pxToHpTime(e.dragCurrent.x);
    this.currentY = e.dragCurrent.y;
  }

  timeSpan() {
    return HighPrecisionTimeSpan.fromHpTimes(this.startTime, this.currentTime);
  }

  rect(timescale: TimeScale) {
    const horizontal = timescale.hpTimeSpanToPxSpan(this.timeSpan());
    return Rect2D.fromPoints(
      {
        x: horizontal.left,
        y: this.startY,
      },
      {
        x: horizontal.right,
        y: this.currentY,
      },
    );
  }
}

// Stores an in-progress handle drag.
class InProgressHandleDrag {
  currentTime: HighPrecisionTime;

  constructor(readonly startTime: HighPrecisionTime) {
    this.currentTime = startTime;
  }

  timeSpan() {
    return HighPrecisionTimeSpan.fromHpTimes(this.startTime, this.currentTime);
  }

  hBounds(timescale: TimeScale): HorizontalBounds {
    const horizontal = timescale.hpTimeSpanToPxSpan(this.timeSpan());
    return new Rect2D({
      ...horizontal,
      top: 0,
      bottom: 0,
    });
  }
}
