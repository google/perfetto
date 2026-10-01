// Copyright (C) 2026 The Android Open Source Project
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

import {DisposableStack} from '../../base/disposable_stack';
import {bindEventListener} from '../../base/dom_utils';

// A 2D pan/zoom controller for a canvas-like element.
//
// The transform maps "base" coordinates (whatever the renderer produces at
// zoom=1, pan=0, in CSS pixels relative to the element) to screen coordinates:
//   screen = base * zoom + pan
//
// Gestures (all zooms are anchored at the cursor / pinch midpoint so the point
// under the pointer stays fixed):
//   - Mouse wheel:                 zoom
//   - Ctrl/Cmd + wheel, trackpad pinch (reported by browsers as ctrl+wheel):
//                                  smooth zoom
//   - Shift + wheel:               horizontal pan
//   - Horizontal trackpad scroll:  pan
//   - Left / middle drag:          pan (a press without movement is a click)
//   - Two-finger touch pinch:      zoom + pan
//   - Keyboard (element focused):  +/= zoom in, - zoom out, 0 reset,
//                                  arrows pan (Shift = faster), F = fit
//                                  selection (delegated to the client).

export interface Point {
  readonly x: number;
  readonly y: number;
}

export interface Rect {
  readonly left: number;
  readonly top: number;
  readonly right: number;
  readonly bottom: number;
}

export interface CanvasPanZoomCallbacks {
  // Transform changed; the client should repaint.
  onChange(): void;
  // Primary-button press + release without dragging. Coordinates are relative
  // to the element.
  onClick?(p: Point, e: PointerEvent): void;
  // Pointer moved while not dragging.
  onHover?(p: Point, e: PointerEvent): void;
  // Pointer left the element (or a drag started).
  onHoverEnd?(): void;
  onDoubleClick?(p: Point, e: MouseEvent): void;
  // 'F' pressed while the element has focus.
  onFitSelection?(): void;
}

export interface CanvasPanZoomOptions {
  readonly minZoom?: number;
  readonly maxZoom?: number;
}

// Pixels a pointer must move before a press becomes a drag.
const DRAG_THRESHOLD_PX = 3;
// Zoom sensitivity per wheel pixel for discrete mouse wheels and for
// ctrl/pinch gestures (which report small, high-frequency deltas).
const WHEEL_ZOOM_SPEED = 0.002;
const PINCH_ZOOM_SPEED = 0.01;
// Largest wheel delta honoured per event, to avoid huge jumps from
// high-resolution or accelerated wheels.
const MAX_WHEEL_DELTA = 120;
const KEY_ZOOM_FACTOR = 1.25;
const KEY_PAN_PX = 40;
const KEY_PAN_FAST_PX = 160;
const LINE_HEIGHT_PX = 16;

export class CanvasPanZoom {
  private _zoom = 1;
  private _panX = 0;
  private _panY = 0;
  private readonly minZoom: number;
  private readonly maxZoom: number;

  private el?: HTMLElement;
  private readonly trash = new DisposableStack();

  // Active pointers (for drag and two-finger pinch), keyed by pointerId.
  private readonly pointers = new Map<number, Point>();
  private pressStart?: Point;
  private dragging = false;
  private lastPinch?: {dist: number; mid: Point};

  constructor(
    private readonly callbacks: CanvasPanZoomCallbacks,
    opts: CanvasPanZoomOptions = {},
  ) {
    this.minZoom = opts.minZoom ?? 0.1;
    this.maxZoom = opts.maxZoom ?? 64;
  }

  get zoom(): number {
    return this._zoom;
  }
  get panX(): number {
    return this._panX;
  }
  get panY(): number {
    return this._panY;
  }
  get isDragging(): boolean {
    return this.dragging;
  }

  toScreen(p: Point): Point {
    return {x: p.x * this._zoom + this._panX, y: p.y * this._zoom + this._panY};
  }

  toBase(p: Point): Point {
    return {
      x: (p.x - this._panX) / this._zoom,
      y: (p.y - this._panY) / this._zoom,
    };
  }

  // Multiplies the zoom by `factor`, keeping `anchor` (element coords) fixed.
  zoomAt(factor: number, anchor: Point): void {
    const next = clamp(this._zoom * factor, this.minZoom, this.maxZoom);
    if (next === this._zoom) return;
    const k = next / this._zoom;
    this._panX = anchor.x - (anchor.x - this._panX) * k;
    this._panY = anchor.y - (anchor.y - this._panY) * k;
    this._zoom = next;
    this.callbacks.onChange();
  }

  // Zooms around the centre of the element.
  zoomCentered(factor: number): void {
    const r = this.el?.getBoundingClientRect();
    this.zoomAt(factor, {x: (r?.width ?? 0) / 2, y: (r?.height ?? 0) / 2});
  }

  panBy(dx: number, dy: number): void {
    if (dx === 0 && dy === 0) return;
    this._panX += dx;
    this._panY += dy;
    this.callbacks.onChange();
  }

  reset(): void {
    this._zoom = 1;
    this._panX = 0;
    this._panY = 0;
    this.callbacks.onChange();
  }

  // Sets the transform so `rect` (base coords) fills `viewport` (element
  // coords) with `padding` px on each side, preserving aspect ratio.
  fitRect(rect: Rect, viewport: Rect, padding = 24): void {
    const rw = Math.max(1, rect.right - rect.left);
    const rh = Math.max(1, rect.bottom - rect.top);
    const vw = Math.max(1, viewport.right - viewport.left - 2 * padding);
    const vh = Math.max(1, viewport.bottom - viewport.top - 2 * padding);
    const zoom = clamp(Math.min(vw / rw, vh / rh), this.minZoom, this.maxZoom);
    const cx = (rect.left + rect.right) / 2;
    const cy = (rect.top + rect.bottom) / 2;
    const vcx = (viewport.left + viewport.right) / 2;
    const vcy = (viewport.top + viewport.bottom) / 2;
    this._zoom = zoom;
    this._panX = vcx - cx * zoom;
    this._panY = vcy - cy * zoom;
    this.callbacks.onChange();
  }

  attach(el: HTMLElement): void {
    this.detach();
    this.el = el;
    // Let us handle touch gestures instead of the browser scrolling the page.
    el.style.touchAction = 'none';
    if (!el.hasAttribute('tabindex')) el.tabIndex = 0;

    this.trash.use(
      bindEventListener(el, 'wheel', this.onWheel, {passive: false}),
    );
    this.trash.use(bindEventListener(el, 'pointerdown', this.onPointerDown));
    this.trash.use(bindEventListener(el, 'pointermove', this.onPointerMove));
    this.trash.use(bindEventListener(el, 'pointerup', this.onPointerUp));
    this.trash.use(
      bindEventListener(el, 'pointercancel', this.onPointerCancel),
    );
    this.trash.use(bindEventListener(el, 'pointerleave', this.onPointerLeave));
    this.trash.use(bindEventListener(el, 'dblclick', this.onDoubleClick));
    this.trash.use(bindEventListener(el, 'keydown', this.onKeyDown));
  }

  detach(): void {
    this.trash.dispose();
    this.pointers.clear();
    this.dragging = false;
    this.pressStart = undefined;
    this.lastPinch = undefined;
    this.el = undefined;
  }

  [Symbol.dispose](): void {
    this.detach();
  }

  private local(e: MouseEvent): Point {
    const r = this.el!.getBoundingClientRect();
    return {x: e.clientX - r.left, y: e.clientY - r.top};
  }

  private readonly onWheel = (e: WheelEvent) => {
    if (!this.el) return;
    e.preventDefault();
    const scale =
      e.deltaMode === WheelEvent.DOM_DELTA_LINE
        ? LINE_HEIGHT_PX
        : e.deltaMode === WheelEvent.DOM_DELTA_PAGE
          ? this.el.clientHeight
          : 1;
    const dx = clamp(e.deltaX * scale, -MAX_WHEEL_DELTA, MAX_WHEEL_DELTA);
    const dy = clamp(e.deltaY * scale, -MAX_WHEEL_DELTA, MAX_WHEEL_DELTA);
    const anchor = this.local(e);

    if (e.ctrlKey || e.metaKey) {
      // Trackpad pinch or explicit ctrl+wheel.
      this.zoomAt(Math.exp(-dy * PINCH_ZOOM_SPEED), anchor);
    } else if (e.shiftKey) {
      // Some browsers already swap the axis for shift+wheel.
      this.panBy(-(dx !== 0 ? dx : dy), 0);
    } else if (Math.abs(dx) > Math.abs(dy)) {
      // Horizontal trackpad swipe.
      this.panBy(-dx, -dy);
    } else {
      this.zoomAt(Math.exp(-dy * WHEEL_ZOOM_SPEED), anchor);
    }
  };

  private readonly onPointerDown = (e: PointerEvent) => {
    if (!this.el) return;
    // Primary (left / touch / pen) or middle button only.
    if (e.pointerType === 'mouse' && e.button !== 0 && e.button !== 1) return;
    if (e.button === 1) e.preventDefault(); // Suppress auto-scroll.
    this.el.setPointerCapture(e.pointerId);
    const p = this.local(e);
    this.pointers.set(e.pointerId, p);
    if (this.pointers.size === 1) {
      this.pressStart = p;
      this.dragging = false;
    } else {
      // Second finger: switch to pinch; a pinch never produces a click.
      this.pressStart = undefined;
      this.dragging = true;
      this.lastPinch = this.pinchState();
      this.callbacks.onHoverEnd?.();
    }
  };

  private readonly onPointerMove = (e: PointerEvent) => {
    const p = this.local(e);
    const prev = this.pointers.get(e.pointerId);
    if (prev === undefined) {
      this.callbacks.onHover?.(p, e);
      return;
    }
    this.pointers.set(e.pointerId, p);

    if (this.pointers.size >= 2) {
      const cur = this.pinchState();
      if (cur && this.lastPinch) {
        this.panBy(
          cur.mid.x - this.lastPinch.mid.x,
          cur.mid.y - this.lastPinch.mid.y,
        );
        if (this.lastPinch.dist > 0) {
          this.zoomAt(cur.dist / this.lastPinch.dist, cur.mid);
        }
      }
      this.lastPinch = cur;
      return;
    }

    if (!this.dragging && this.pressStart !== undefined) {
      const moved = Math.hypot(
        p.x - this.pressStart.x,
        p.y - this.pressStart.y,
      );
      if (moved <= DRAG_THRESHOLD_PX) return;
      this.dragging = true;
      this.callbacks.onHoverEnd?.();
      // Include the movement accumulated below the threshold.
      this.panBy(p.x - this.pressStart.x, p.y - this.pressStart.y);
      return;
    }
    if (this.dragging) this.panBy(p.x - prev.x, p.y - prev.y);
  };

  private readonly onPointerUp = (e: PointerEvent) => {
    if (!this.pointers.has(e.pointerId)) return;
    this.el?.releasePointerCapture(e.pointerId);
    this.pointers.delete(e.pointerId);
    const wasClick =
      this.pointers.size === 0 &&
      !this.dragging &&
      this.pressStart !== undefined &&
      e.button === 0;
    if (this.pointers.size < 2) this.lastPinch = undefined;
    if (this.pointers.size === 0) {
      const wasDragging = this.dragging;
      this.dragging = false;
      this.pressStart = undefined;
      if (wasDragging) this.callbacks.onChange();
    }
    if (wasClick) this.callbacks.onClick?.(this.local(e), e);
  };

  private readonly onPointerCancel = (e: PointerEvent) => {
    this.pointers.delete(e.pointerId);
    if (this.pointers.size === 0) {
      this.dragging = false;
      this.pressStart = undefined;
      this.lastPinch = undefined;
      this.callbacks.onChange();
    }
  };

  private readonly onPointerLeave = () => {
    if (this.pointers.size === 0) this.callbacks.onHoverEnd?.();
  };

  private readonly onDoubleClick = (e: MouseEvent) => {
    this.callbacks.onDoubleClick?.(this.local(e), e);
  };

  private readonly onKeyDown = (e: KeyboardEvent) => {
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    const step = e.shiftKey ? KEY_PAN_FAST_PX : KEY_PAN_PX;
    switch (e.key) {
      case '+':
      case '=':
        this.zoomCentered(KEY_ZOOM_FACTOR);
        break;
      case '-':
      case '_':
        this.zoomCentered(1 / KEY_ZOOM_FACTOR);
        break;
      case '0':
        this.reset();
        break;
      case 'ArrowLeft':
        this.panBy(step, 0);
        break;
      case 'ArrowRight':
        this.panBy(-step, 0);
        break;
      case 'ArrowUp':
        this.panBy(0, step);
        break;
      case 'ArrowDown':
        this.panBy(0, -step);
        break;
      case 'f':
      case 'F':
        this.callbacks.onFitSelection?.();
        break;
      default:
        return;
    }
    e.preventDefault();
    e.stopPropagation();
  };

  private pinchState(): {dist: number; mid: Point} | undefined {
    if (this.pointers.size < 2) return undefined;
    const [a, b] = this.pointers.values();
    return {
      dist: Math.hypot(a.x - b.x, a.y - b.y),
      mid: {x: (a.x + b.x) / 2, y: (a.y + b.y) / 2},
    };
  }
}

function clamp(v: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(hi, v));
}
