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

import m from 'mithril';
import {canvasSave} from '../../base/canvas_utils';
import {DisposableStack} from '../../base/disposable_stack';
import {toHTMLElement} from '../../base/dom_utils';
import {Rect2D, Size2D} from '../../base/geom';
import {assertExists} from '../../base/logging';
import {TimeScale} from '../../base/time_scale';
import {ZonedInteractionHandler} from '../../base/zoned_interaction_handler';
import {TraceImpl} from '../../core/trace_impl';
import {
  VirtualOverlayCanvas,
  VirtualOverlayCanvasDrawContext,
} from '../../widgets/virtual_overlay_canvas';
import {TRACK_SHELL_WIDTH} from '../css_constants';
import {NotesPanel} from './notes_panel';
import {TickmarkPanel} from './tickmark_panel';
import {TimeAxisPanel} from './time_axis_panel';
import {TimeSelectionPanel} from './time_selection_panel';
import {
  shiftDragPanInteraction,
  wheelNavigationInteraction,
} from './timeline_interactions';

export interface TimelineHeaderAttrs {
  // The trace to use for timeline access et al.
  readonly trace: TraceImpl;

  // Called when the visible area of the timeline changes size. This is the area
  // to the right of the header is actually rendered on.
  onTimelineBoundsChange?(rect: Rect2D): void;

  readonly className?: string;
}

// TODO(stevegolton): The panel concept has been largely removed. It's just
// defined here so that we don't have to change the implementation of the
// various header panels listed here. We should consolidate this in the future.
interface Panel {
  readonly height: number;
  render(): m.Children;
  renderCanvas(ctx: CanvasRenderingContext2D, size: Size2D): void;
}

/**
 * This component defines the header of the timeline and handles it's mouse
 * interactions.
 *
 * The timeline header contains:
 * - The axis (ticks) and time labels
 * - The selection bar
 * - The notes bar
 * - The tickmark bar (highlights that appear when searching)
 */
export class TimelineHeader implements m.ClassComponent<TimelineHeaderAttrs> {
  private readonly trash = new DisposableStack();
  private readonly trace: TraceImpl;
  private readonly panels: ReadonlyArray<Panel>;
  private interactions?: ZonedInteractionHandler;

  constructor({attrs}: m.Vnode<TimelineHeaderAttrs>) {
    this.trace = attrs.trace;
    this.panels = [
      new TimeAxisPanel(attrs.trace),
      new TimeSelectionPanel(attrs.trace),
      new NotesPanel(attrs.trace),
      new TickmarkPanel(attrs.trace),
    ];
  }

  view({attrs}: m.Vnode<TimelineHeaderAttrs>) {
    return m(
      '.pf-timeline-header',
      {className: attrs.className},
      m(
        VirtualOverlayCanvas,
        {
          onMount: (redrawCanvas) =>
            attrs.trace.raf.addCanvasRedrawCallback(redrawCanvas),
          disableCanvasRedrawOnMithrilUpdates: true,
          onCanvasRedraw: (ctx) => {
            const rect = new Rect2D({
              left: TRACK_SHELL_WIDTH,
              right: ctx.virtualCanvasSize.width,
              top: 0,
              bottom: 0,
            });
            attrs.onTimelineBoundsChange?.(rect);
            this.drawCanvas(ctx);
          },
        },
        this.panels.map((p) => p.render()),
      ),
    );
  }

  oncreate({dom}: m.VnodeDOM<TimelineHeaderAttrs>) {
    const timelineHeaderElement = toHTMLElement(dom);
    this.interactions = new ZonedInteractionHandler(timelineHeaderElement);
    this.trash.use(this.interactions);
  }

  onremove() {
    this.trash.dispose();
  }

  private drawCanvas({
    ctx,
    virtualCanvasSize,
  }: VirtualOverlayCanvasDrawContext) {
    let top = 0;
    for (const p of this.panels) {
      using _ = canvasSave(ctx);
      ctx.translate(0, top);
      p.renderCanvas(ctx, {width: virtualCanvasSize.width, height: p.height});
      top += p.height;
    }

    const timelineRect = new Rect2D({
      left: TRACK_SHELL_WIDTH,
      top: 0,
      right: virtualCanvasSize.width,
      bottom: virtualCanvasSize.height,
    });

    // Always grab the latest visible window and create a timescale
    // out of it.
    const visibleWindow = this.trace.timeline.visibleWindow;
    const timescale = new TimeScale(visibleWindow, timelineRect);

    assertExists(this.interactions).update([
      shiftDragPanInteraction(this.trace, timelineRect, timescale),
      wheelNavigationInteraction(this.trace, timelineRect, timescale),
      {
        // Allow making area selections (no tracks) by dragging on the header
        // timeline.
        id: 'area-selection',
        area: timelineRect,
        drag: {
          minDistance: 1,
          cursorWhileDragging: 'text',
          onDrag: (e) => {
            this.trace.raf.scheduleCanvasRedraw();
            const dragRect = Rect2D.fromPoints(e.dragStart, e.dragCurrent);
            const timeSpan = timescale
              .pxSpanToHpTimeSpan(dragRect)
              .toTimeSpan();
            this.trace.timeline.selectedSpan = timeSpan;
          },
          onDragEnd: (e) => {
            const dragRect = Rect2D.fromPoints(e.dragStart, e.dragCurrent);
            const timeSpan = timescale
              .pxSpanToHpTimeSpan(dragRect)
              .toTimeSpan();
            this.trace.selection.selectArea({
              start: timeSpan.start,
              end: timeSpan.end,
              trackUris: [],
            });
            this.trace.timeline.selectedSpan = undefined;
          },
        },
      },
    ]);
  }
}
