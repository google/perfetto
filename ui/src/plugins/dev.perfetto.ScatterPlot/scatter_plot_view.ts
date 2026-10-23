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

import m from 'mithril';
import {DisposableStack} from '../../base/disposable_stack';
import {bindEventListener} from '../../base/dom_utils';
import {clamp} from '../../base/math_utils';
import {SimpleResizeObserver} from '../../base/resize_observer';
import {Icons} from '../../base/semantic_icons';
import {KeyMapping} from '../../base/wasd_key_mapping';
import {fitTickCount, formatAxisOffset, formatValue} from './axis';
import {Button, ButtonVariant} from '../../widgets/button';
import {CursorTooltip} from '../../widgets/cursor_tooltip';
import {
  buildColorEncoding,
  gradientCss,
  legendEntries,
  readScatterTheme,
  type ScatterTheme,
} from './color_scale';
import type {PointRef, ScatterController} from './scatter_controller';
import type {ColorEncoding, PointStyle, ViewRange} from './types';
import {
  clampView,
  dataToScreenX,
  dataToScreenY,
  panView,
  screenToDataX,
  screenToDataY,
  zoomView,
} from './view';
import {type HighlightPoint, WebGLScatterRenderer} from './webgl_renderer';

export interface ScatterPlotViewAttrs {
  readonly controller: ScatterController;
  readonly pointStyle?: PointStyle;
}

const MARGIN_LEFT = 64;
const MARGIN_BOTTOM = 36;
const MARGIN_TOP = 12;
const MARGIN_RIGHT = 12;

// Zoom factor per W/S (or +/-) key press; key repeat accumulates smoothly.
const KEY_ZOOM_FACTOR = 1.3;
// Fraction of the visible span panned per A/D/arrow key press.
const KEY_PAN_FRACTION = 0.1;

const DEFAULT_STYLE: PointStyle = {
  sizePx: 3,
  opacity: 0.7,
};

interface DragState {
  readonly startX: number;
  readonly startY: number;
  readonly startView: ViewRange;
  readonly isShift: boolean;
  currentX: number;
  currentY: number;
  hasMoved: boolean;
}

/**
 * Interactive WebGL2 scatter plot component with Canvas2D overlay.
 */
export class ScatterPlotView implements m.ClassComponent<ScatterPlotViewAttrs> {
  private containerDom?: HTMLElement;
  private plotCanvas?: HTMLCanvasElement;
  private overlayCanvas?: HTMLCanvasElement;
  private renderer?: WebGLScatterRenderer;

  private readonly trash = new DisposableStack();
  private rafId?: number;
  private isDirty = true;

  private containerWidth = 0;
  private containerHeight = 0;
  private dpr = 1;

  private cachedRect?: DOMRect;
  private legendCollapsed = false;

  private drag?: DragState;
  private pendingHoverPointer?: {clientX: number; clientY: number};

  private targetView?: ViewRange;
  private animRafId?: number;
  private lastAnimTime?: number;
  private lastWheelCursor?: {x: number; y: number};
  // Last pointer position in plot-relative px while the pointer is over the
  // plot area; used as the keyboard zoom anchor.
  private lastPointerPx?: {x: number; y: number};
  private pointerInside = false;

  private cachedEncoding?: ColorEncoding;
  private lastColorModeJson = '';
  private lastHiddenKey = '';
  private lastEncodingTheme?: ScatterTheme;

  private cachedHighlightRgba?: Uint8Array;
  private lastBgLum = -1;

  private cachedTheme?: ScatterTheme;
  private cachedThemeClass = '';

  private currentController?: ScatterController;
  private currentPointStyle: PointStyle = DEFAULT_STYLE;

  private getTheme(): ScatterTheme {
    const rootClass =
      (typeof document !== 'undefined' && document.documentElement.className) ||
      '';
    if (this.cachedTheme === undefined || this.cachedThemeClass !== rootClass) {
      this.cachedThemeClass = rootClass;
      this.cachedTheme = readScatterTheme(
        this.containerDom ?? document.documentElement,
      );
    }
    return this.cachedTheme;
  }

  oncreate(vnode: m.CVnodeDOM<ScatterPlotViewAttrs>) {
    this.containerDom = vnode.dom as HTMLElement;
    this.currentController = vnode.attrs.controller;
    this.currentPointStyle = vnode.attrs.pointStyle ?? DEFAULT_STYLE;

    this.trash.use(
      bindEventListener(
        window,
        'scroll',
        () => {
          if (this.containerDom !== undefined) {
            this.cachedRect = this.containerDom.getBoundingClientRect();
          }
        },
        {passive: true, capture: true},
      ),
    );

    this.trash.use(
      bindEventListener(document, 'keydown', (e: KeyboardEvent) =>
        this.onDocumentKeyDown(e),
      ),
    );

    this.trash.use(
      new SimpleResizeObserver(this.containerDom, () => {
        this.handleResize();
      }),
    );
    this.handleResize();
    this.syncDomDataAttributes();
    this.scheduleRedraw();
  }

  onupdate(vnode: m.CVnodeDOM<ScatterPlotViewAttrs>) {
    if (vnode.dom instanceof HTMLElement) {
      this.containerDom = vnode.dom;
    }
    this.currentController = vnode.attrs.controller;
    this.currentPointStyle = vnode.attrs.pointStyle ?? DEFAULT_STYLE;
    this.scheduleRedraw();
  }

  onremove() {
    if (this.animRafId !== undefined) {
      cancelAnimationFrame(this.animRafId);
      this.animRafId = undefined;
    }
    if (this.rafId !== undefined) {
      cancelAnimationFrame(this.rafId);
      this.rafId = undefined;
    }
    this.trash.dispose();
    this.renderer?.[Symbol.dispose]();
    this.renderer = undefined;
  }

  private getContainerRect(forceRefresh = false): DOMRect {
    if (this.cachedRect === undefined || forceRefresh) {
      if (this.containerDom !== undefined) {
        this.cachedRect = this.containerDom.getBoundingClientRect();
      }
    }
    return (
      this.cachedRect ??
      new DOMRect(0, 0, this.containerWidth, this.containerHeight)
    );
  }

  private handleResize(): void {
    if (this.containerDom === undefined) {
      return;
    }
    const rect = this.getContainerRect(true);
    const newWidth = Math.max(0, Math.floor(rect.width));
    const newHeight = Math.max(0, Math.floor(rect.height));
    const newDpr = window.devicePixelRatio || 1;

    if (
      newWidth === this.containerWidth &&
      newHeight === this.containerHeight &&
      newDpr === this.dpr
    ) {
      return;
    }

    this.containerWidth = newWidth;
    this.containerHeight = newHeight;
    this.dpr = newDpr;

    const plotW = Math.max(1, this.containerWidth - MARGIN_LEFT - MARGIN_RIGHT);
    const plotH = Math.max(
      1,
      this.containerHeight - MARGIN_TOP - MARGIN_BOTTOM,
    );
    if (this.plotCanvas !== undefined) {
      this.plotCanvas.style.width = `${plotW}px`;
      this.plotCanvas.style.height = `${plotH}px`;
    }

    if (this.renderer !== undefined) {
      this.renderer.resize(plotW, plotH, this.dpr);
    }

    if (this.overlayCanvas !== undefined) {
      this.overlayCanvas.style.width = `${this.containerWidth}px`;
      this.overlayCanvas.style.height = `${this.containerHeight}px`;
      const targetW = Math.round(this.containerWidth * this.dpr);
      const targetH = Math.round(this.containerHeight * this.dpr);
      if (
        this.overlayCanvas.width !== targetW ||
        this.overlayCanvas.height !== targetH
      ) {
        this.overlayCanvas.width = targetW;
        this.overlayCanvas.height = targetH;
      }
    }

    this.currentController?.setPlotSize(
      plotW,
      plotH,
      this.currentPointStyle.sizePx,
    );

    this.isDirty = true;
    this.scheduleRedraw();
    m.redraw();
  }

  private initRenderer(canvas: HTMLCanvasElement): void {
    this.renderer?.[Symbol.dispose]();
    this.renderer = WebGLScatterRenderer.create(canvas);
    if (this.renderer !== undefined) {
      this.renderer.onContextRestoredCallback = () => {
        this.scheduleRedraw();
      };
      const plotW = Math.max(
        1,
        this.containerWidth - MARGIN_LEFT - MARGIN_RIGHT,
      );
      const plotH = Math.max(
        1,
        this.containerHeight - MARGIN_TOP - MARGIN_BOTTOM,
      );
      this.renderer.resize(plotW, plotH, this.dpr);
    }
  }

  private scheduleRedraw(): void {
    this.isDirty = true;
    if (this.rafId !== undefined) {
      return;
    }
    this.rafId = requestAnimationFrame(() => {
      this.rafId = undefined;
      if (this.isDirty) {
        this.isDirty = false;
        this.drawFrame();
      }
    });
  }

  private getPlotArea(): {
    plotWidth: number;
    plotHeight: number;
  } {
    return {
      plotWidth: Math.max(1, this.containerWidth - MARGIN_LEFT - MARGIN_RIGHT),
      plotHeight: Math.max(
        1,
        this.containerHeight - MARGIN_TOP - MARGIN_BOTTOM,
      ),
    };
  }

  private syncDomDataAttributes(): void {
    if (this.containerDom === undefined) {
      return;
    }
    this.containerDom.dataset.renderedPoints = String(
      this.renderer?.lastDrawnPoints ?? 0,
    );
    this.containerDom.dataset.renderer = this.renderer?.kind ?? 'none';
    const v = this.currentController?.view;
    if (v !== undefined) {
      this.containerDom.dataset.view = `${v.x0},${v.x1},${v.y0},${v.y1}`;
    } else {
      delete this.containerDom.dataset.view;
    }
  }

  private drawFrame(): void {
    if (
      this.containerDom === undefined ||
      this.overlayCanvas === undefined ||
      this.plotCanvas === undefined
    ) {
      return;
    }

    const {plotWidth, plotHeight} = this.getPlotArea();
    if (plotWidth <= 0 || plotHeight <= 0) {
      return;
    }

    if (this.renderer === undefined) {
      this.initRenderer(this.plotCanvas);
    }
    if (this.renderer?.isContextLost()) {
      return;
    }

    const theme = this.getTheme();

    if (
      this.currentController !== undefined &&
      this.currentController.view !== undefined
    ) {
      const ctrl = this.currentController;
      const view = ctrl.view;
      if (view !== undefined) {
        // Throttled hover picking in rAF
        if (this.drag === undefined && this.pendingHoverPointer !== undefined) {
          const rect = this.containerDom.getBoundingClientRect();
          const px = this.pendingHoverPointer.clientX - rect.left - MARGIN_LEFT;
          const py = this.pendingHoverPointer.clientY - rect.top - MARGIN_TOP;

          if (px >= 0 && px <= plotWidth && py >= 0 && py <= plotHeight) {
            const dataX = screenToDataX(px, view, plotWidth);
            const dataY = screenToDataY(py, view, plotHeight);
            const scaleX = plotWidth / (view.x1 - view.x0);
            const scaleY = plotHeight / (view.y1 - view.y0);
            const ref = ctrl.pick(dataX, dataY, scaleX, scaleY, 8);
            ctrl.setHover(ref);
          } else {
            ctrl.setHover(undefined);
          }
        }

        // Update color encoding if the mode, hidden set or theme changed.
        const modeJson = JSON.stringify(ctrl.colorMode);
        const hiddenKey = Array.from(ctrl.hiddenCategories)
          .sort((a, b) => a - b)
          .join(',');
        if (
          this.cachedEncoding === undefined ||
          this.lastColorModeJson !== modeJson ||
          this.lastHiddenKey !== hiddenKey ||
          this.lastEncodingTheme !== theme
        ) {
          this.cachedEncoding = buildColorEncoding(
            ctrl.colorMode,
            theme,
            ctrl.hiddenCategories,
          );
          this.lastColorModeJson = modeJson;
          this.lastHiddenKey = hiddenKey;
          this.lastEncodingTheme = theme;
        }

        // Cache highlight ring color based on background luminance
        const [br, bg, bb] = theme.background;
        const lum = (0.299 * br + 0.587 * bg + 0.114 * bb) / 255;
        if (this.cachedHighlightRgba === undefined || this.lastBgLum !== lum) {
          this.cachedHighlightRgba =
            lum > 0.5
              ? new Uint8Array([20, 20, 20, 255])
              : new Uint8Array([245, 245, 245, 255]);
          this.lastBgLum = lum;
        }

        let highlight: HighlightPoint | undefined;
        const pinOrHover = ctrl.pinned ?? ctrl.hover;
        if (pinOrHover !== undefined) {
          highlight = {
            x: pinOrHover.points.x[pinOrHover.index],
            y: pinOrHover.points.y[pinOrHover.index],
            rgba: this.cachedHighlightRgba,
          };
        }

        this.renderer?.render({
          view,
          points: ctrl.points,
          style: this.currentPointStyle,
          color: this.cachedEncoding,
          background: theme.background,
          highlight,
        });

        this.drawOverlay(view, ctrl, theme, plotWidth, plotHeight);
      }
    } else {
      // Nothing to plot (e.g. source without numeric axes): wipe the previous
      // frame so stale points and axes don't linger.
      this.renderer?.clear(theme.background);
      const ctx = this.overlayCanvas.getContext('2d');
      ctx?.clearRect(0, 0, this.overlayCanvas.width, this.overlayCanvas.height);
    }

    this.syncDomDataAttributes();
  }

  private drawOverlay(
    view: ViewRange,
    ctrl: ScatterController,
    theme: ScatterTheme,
    plotWidth: number,
    plotHeight: number,
  ): void {
    const canvas = this.overlayCanvas;
    if (canvas === null || canvas === undefined) {
      return;
    }
    const targetW = Math.max(1, Math.round(this.containerWidth * this.dpr));
    const targetH = Math.max(1, Math.round(this.containerHeight * this.dpr));
    if (canvas.width !== targetW || canvas.height !== targetH) {
      canvas.width = targetW;
      canvas.height = targetH;
    }
    const ctx = canvas.getContext('2d');
    if (ctx === null) {
      return;
    }

    ctx.save();
    ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
    ctx.clearRect(0, 0, this.containerWidth, this.containerHeight);

    // Compute fitted ticks and offsets
    const maxTargetX = Math.max(2, Math.min(12, Math.floor(plotWidth / 60)));
    const measureLabelsX = (labels: string[]): number => {
      let maxW = 0;
      for (const l of labels) {
        maxW = Math.max(maxW, ctx.measureText(l).width);
      }
      return maxW;
    };
    const axisTicksX = fitTickCount(
      view.x0,
      view.x1,
      maxTargetX,
      measureLabelsX,
      plotWidth,
      12,
    );

    const maxTargetY = Math.max(2, Math.min(10, Math.floor(plotHeight / 40)));
    const axisTicksY = fitTickCount(
      view.y0,
      view.y1,
      maxTargetY,
      () => 14,
      plotHeight,
      8,
    );

    // Clip grid lines to plot area
    ctx.save();
    ctx.beginPath();
    ctx.rect(MARGIN_LEFT, MARGIN_TOP, plotWidth, plotHeight);
    ctx.clip();

    ctx.strokeStyle = theme.grid;
    ctx.lineWidth = 1;

    // Vertical grid lines
    for (const tick of axisTicksX.ticks) {
      const sx = MARGIN_LEFT + dataToScreenX(tick, view, plotWidth);
      ctx.beginPath();
      ctx.moveTo(Math.round(sx) + 0.5, MARGIN_TOP);
      ctx.lineTo(Math.round(sx) + 0.5, MARGIN_TOP + plotHeight);
      ctx.stroke();
    }

    // Horizontal grid lines
    for (const tick of axisTicksY.ticks) {
      const sy = MARGIN_TOP + dataToScreenY(tick, view, plotHeight);
      ctx.beginPath();
      ctx.moveTo(MARGIN_LEFT, Math.round(sy) + 0.5);
      ctx.lineTo(MARGIN_LEFT + plotWidth, Math.round(sy) + 0.5);
      ctx.stroke();
    }

    // Brush rectangle inside plot area
    const brush = ctrl.brush?.rect;
    if (this.drag !== undefined && this.drag.isShift && this.drag.hasMoved) {
      const x1 = Math.min(this.drag.startX, this.drag.currentX);
      const x2 = Math.max(this.drag.startX, this.drag.currentX);
      const y1 = Math.min(this.drag.startY, this.drag.currentY);
      const y2 = Math.max(this.drag.startY, this.drag.currentY);
      ctx.fillStyle = 'rgba(66, 133, 244, 0.2)';
      ctx.strokeStyle = 'rgba(66, 133, 244, 0.8)';
      ctx.lineWidth = 1.5;
      ctx.fillRect(x1, y1, x2 - x1, y2 - y1);
      ctx.strokeRect(x1, y1, x2 - x1, y2 - y1);
    } else if (brush !== undefined) {
      const bx0 = MARGIN_LEFT + dataToScreenX(brush.xMin, view, plotWidth);
      const bx1 = MARGIN_LEFT + dataToScreenX(brush.xMax, view, plotWidth);
      const by0 = MARGIN_TOP + dataToScreenY(brush.yMax, view, plotHeight);
      const by1 = MARGIN_TOP + dataToScreenY(brush.yMin, view, plotHeight);
      const rx = Math.min(bx0, bx1);
      const ry = Math.min(by0, by1);
      const rw = Math.abs(bx1 - bx0);
      const rh = Math.abs(by1 - by0);
      ctx.fillStyle = 'rgba(66, 133, 244, 0.15)';
      ctx.strokeStyle = 'rgba(66, 133, 244, 0.8)';
      ctx.lineWidth = 1.5;
      ctx.fillRect(rx, ry, rw, rh);
      ctx.strokeRect(rx, ry, rw, rh);
    }

    ctx.restore(); // Exit clip

    // 2. Plot boundary
    ctx.strokeStyle = theme.grid;
    ctx.lineWidth = 1;
    ctx.strokeRect(MARGIN_LEFT + 0.5, MARGIN_TOP + 0.5, plotWidth, plotHeight);

    // 3. Tick Labels and Axis Titles
    ctx.font = '10px Roboto, sans-serif';
    ctx.fillStyle = theme.text;

    // X Tick Labels
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (let i = 0; i < axisTicksX.ticks.length; i++) {
      const tick = axisTicksX.ticks[i];
      const label = axisTicksX.labels[i];
      const sx = MARGIN_LEFT + dataToScreenX(tick, view, plotWidth);
      if (sx >= MARGIN_LEFT - 10 && sx <= MARGIN_LEFT + plotWidth + 10) {
        ctx.fillText(label, sx, MARGIN_TOP + plotHeight + 6);
      }
    }

    // Y Tick Labels
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    const maxAllowedLabelW = MARGIN_LEFT - 10;
    for (let i = 0; i < axisTicksY.ticks.length; i++) {
      const tick = axisTicksY.ticks[i];
      let label = axisTicksY.labels[i];
      const sy = MARGIN_TOP + dataToScreenY(tick, view, plotHeight);
      if (sy >= MARGIN_TOP - 10 && sy <= MARGIN_TOP + plotHeight + 10) {
        if (ctx.measureText(label).width > maxAllowedLabelW) {
          while (
            label.length > 3 &&
            ctx.measureText(label + '…').width > maxAllowedLabelW
          ) {
            label = label.slice(0, -1);
          }
          label = label + '…';
        }
        ctx.fillText(label, MARGIN_LEFT - 8, sy);
      }
    }

    // Axis titles with offset notation if present
    ctx.font = '11px Roboto, sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'bottom';
    const baseColX = ctrl.plot?.x ?? 'X';
    const xTitle =
      axisTicksX.offset !== 0
        ? `${baseColX} (+${formatAxisOffset(axisTicksX.offset)})`
        : baseColX;
    ctx.fillText(
      xTitle,
      MARGIN_LEFT + plotWidth * 0.5,
      this.containerHeight - 2,
    );

    ctx.save();
    ctx.translate(14, MARGIN_TOP + plotHeight * 0.5);
    ctx.rotate(-Math.PI / 2);
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    const baseColY = ctrl.plot?.y ?? 'Y';
    const yTitle =
      axisTicksY.offset !== 0
        ? `${baseColY} (+${formatAxisOffset(axisTicksY.offset)})`
        : baseColY;
    ctx.fillText(yTitle, 0, 0);
    ctx.restore();

    ctx.restore();
  }

  private cancelZoomAnimation(): void {
    if (this.animRafId !== undefined) {
      cancelAnimationFrame(this.animRafId);
      this.animRafId = undefined;
    }
    this.targetView = undefined;
    this.lastWheelCursor = undefined;
  }

  private startZoomAnimation(ctrl: ScatterController): void {
    if (this.animRafId !== undefined) {
      return;
    }
    this.lastAnimTime = performance.now();
    const step = (now: number) => {
      this.animRafId = undefined;
      if (this.targetView === undefined || ctrl.view === undefined) {
        return;
      }
      const dt = Math.max(1, Math.min(100, now - (this.lastAnimTime ?? now)));
      this.lastAnimTime = now;
      const alpha = 1 - Math.exp(-dt / 70);

      const curr = ctrl.view;
      const target = this.targetView;

      const {plotWidth, plotHeight} = this.getPlotArea();
      const spanX = curr.x1 - curr.x0;
      const spanY = curr.y1 - curr.y0;
      const pxDiffX0 = (Math.abs(target.x0 - curr.x0) / spanX) * plotWidth;
      const pxDiffX1 = (Math.abs(target.x1 - curr.x1) / spanX) * plotWidth;
      const pxDiffY0 = (Math.abs(target.y0 - curr.y0) / spanY) * plotHeight;
      const pxDiffY1 = (Math.abs(target.y1 - curr.y1) / spanY) * plotHeight;
      const maxPxDiff = Math.max(pxDiffX0, pxDiffX1, pxDiffY0, pxDiffY1);

      if (maxPxDiff < 0.5 || alpha >= 0.99) {
        ctrl.setView(target);
        this.targetView = undefined;
        this.lastWheelCursor = undefined;
        ctrl.flushViewport();
        this.scheduleRedraw();
        return;
      }

      const nextView: ViewRange = {
        x0: curr.x0 + alpha * (target.x0 - curr.x0),
        x1: curr.x1 + alpha * (target.x1 - curr.x1),
        y0: curr.y0 + alpha * (target.y0 - curr.y0),
        y1: curr.y1 + alpha * (target.y1 - curr.y1),
      };

      ctrl.setView(nextView);
      this.scheduleRedraw();
      this.animRafId = requestAnimationFrame(step);
    };
    this.animRafId = requestAnimationFrame(step);
  }

  private onWheel(e: WheelEvent, ctrl: ScatterController): void {
    e.preventDefault();
    if (
      this.containerDom === undefined ||
      ctrl.view === undefined ||
      ctrl.dataBounds === undefined
    ) {
      return;
    }

    const rect = this.containerDom.getBoundingClientRect();
    const cursorX = e.clientX - rect.left - MARGIN_LEFT;
    const cursorY = e.clientY - rect.top - MARGIN_TOP;
    const {plotWidth, plotHeight} = this.getPlotArea();

    if (
      cursorX < 0 ||
      cursorX > plotWidth ||
      cursorY < 0 ||
      cursorY > plotHeight
    ) {
      return;
    }

    if (
      this.lastWheelCursor === undefined ||
      Math.hypot(
        cursorX - this.lastWheelCursor.x,
        cursorY - this.lastWheelCursor.y,
      ) > 1
    ) {
      this.targetView = undefined;
      this.lastWheelCursor = {x: cursorX, y: cursorY};
    }

    let delta = e.deltaY;
    if (e.deltaMode === 1) {
      delta *= 20;
    } else if (e.deltaMode === 2) {
      delta *= 500;
    }

    const rawFactor = Math.pow(2, -delta * 0.002);
    let factorX = rawFactor;
    let factorY = rawFactor;
    if (e.shiftKey && !e.altKey) {
      factorY = 1;
    } else if (e.altKey && !e.shiftKey) {
      factorX = 1;
    }

    const isTrackpad = e.ctrlKey || (Math.abs(delta) < 15 && e.deltaMode === 0);
    this.zoomAt(ctrl, factorX, factorY, {x: cursorX, y: cursorY}, !isTrackpad);
  }

  // Zooms by (factorX, factorY) (> 1 zooms in) keeping the data point under
  // `anchorPx` (plot-relative pixels) fixed on screen. Falls back to the view
  // centre when no anchor is given. When `animate` is set, the zoom is
  // accumulated into `targetView` and eased towards; otherwise it is applied
  // immediately.
  private zoomAt(
    ctrl: ScatterController,
    factorX: number,
    factorY: number,
    anchorPx: {x: number; y: number} | undefined,
    animate: boolean,
  ): void {
    if (ctrl.view === undefined || ctrl.dataBounds === undefined) {
      return;
    }
    const base = this.targetView ?? ctrl.view;
    const bounds = ctrl.dataBounds;
    const totalSpanX = Math.max(1e-9, bounds.xMax - bounds.xMin);
    const totalSpanY = Math.max(1e-9, bounds.yMax - bounds.yMin);
    const minSpanX = totalSpanX * (1 / 1048576);
    const maxSpanX = totalSpanX * 4;
    const minSpanY = totalSpanY * (1 / 1048576);
    const maxSpanY = totalSpanY * 4;
    const curSpanX = base.x1 - base.x0;
    const curSpanY = base.y1 - base.y0;

    let fx = factorX;
    let fy = factorY;
    if (fx !== 1) {
      fx = clamp(fx, curSpanX / maxSpanX, curSpanX / minSpanX);
    }
    if (fy !== 1) {
      fy = clamp(fy, curSpanY / maxSpanY, curSpanY / minSpanY);
    }
    if (Math.abs(fx - 1) < 1e-7 && Math.abs(fy - 1) < 1e-7) {
      return;
    }

    this.pendingHoverPointer = undefined;
    ctrl.setHover(undefined);

    const {plotWidth, plotHeight} = this.getPlotArea();
    const anchorX =
      anchorPx !== undefined
        ? screenToDataX(anchorPx.x, base, plotWidth)
        : (base.x0 + base.x1) * 0.5;
    const anchorY =
      anchorPx !== undefined
        ? screenToDataY(anchorPx.y, base, plotHeight)
        : (base.y0 + base.y1) * 0.5;

    const next = clampView(zoomView(base, fx, fy, anchorX, anchorY), bounds);
    if (animate) {
      this.targetView = next;
      this.startZoomAnimation(ctrl);
    } else {
      this.cancelZoomAnimation();
      ctrl.setView(next);
      this.scheduleRedraw();
    }
  }

  private onPointerDown(e: PointerEvent, ctrl: ScatterController): void {
    if (this.containerDom === undefined || ctrl.view === undefined) {
      return;
    }

    this.cancelZoomAnimation();

    this.containerDom.setPointerCapture(e.pointerId);
    const rect = this.containerDom.getBoundingClientRect();
    const {plotWidth, plotHeight} = this.getPlotArea();
    const containerX = Math.max(
      MARGIN_LEFT,
      Math.min(MARGIN_LEFT + plotWidth, e.clientX - rect.left),
    );
    const containerY = Math.max(
      MARGIN_TOP,
      Math.min(MARGIN_TOP + plotHeight, e.clientY - rect.top),
    );

    this.drag = {
      startX: containerX,
      startY: containerY,
      currentX: containerX,
      currentY: containerY,
      startView: ctrl.view,
      isShift: e.shiftKey,
      hasMoved: false,
    };
    this.pendingHoverPointer = undefined;
  }

  private onPointerMove(e: PointerEvent, ctrl: ScatterController): void {
    if (this.containerDom === undefined || ctrl.view === undefined) {
      return;
    }
    const rect = this.containerDom.getBoundingClientRect();
    const {plotWidth, plotHeight} = this.getPlotArea();
    const containerX = e.clientX - rect.left;
    const containerY = e.clientY - rect.top;
    const plotX = containerX - MARGIN_LEFT;
    const plotY = containerY - MARGIN_TOP;
    const inPlot =
      plotX >= 0 && plotX <= plotWidth && plotY >= 0 && plotY <= plotHeight;
    this.pointerInside = true;
    this.lastPointerPx = inPlot ? {x: plotX, y: plotY} : undefined;

    if (this.drag !== undefined) {
      const dx = containerX - this.drag.startX;
      const dy = containerY - this.drag.startY;
      if (!this.drag.hasMoved && Math.hypot(dx, dy) > 3) {
        this.drag.hasMoved = true;
        this.pendingHoverPointer = undefined;
        ctrl.setHover(undefined);
      }

      if (this.drag.hasMoved) {
        if (this.drag.isShift) {
          // Brush box selection drag clamped to plot area
          this.drag.currentX = clamp(
            containerX,
            MARGIN_LEFT,
            MARGIN_LEFT + plotWidth,
          );
          this.drag.currentY = clamp(
            containerY,
            MARGIN_TOP,
            MARGIN_TOP + plotHeight,
          );
          this.scheduleRedraw();
        } else {
          // Pan
          const spanX = this.drag.startView.x1 - this.drag.startView.x0;
          const spanY = this.drag.startView.y1 - this.drag.startView.y0;
          const dxData = (-dx / plotWidth) * spanX;
          const dyData = (dy / plotHeight) * spanY;
          ctrl.setView(panView(this.drag.startView, dxData, dyData));
          this.scheduleRedraw();
        }
      }
    } else {
      // Store pending hover pointer; pick inside drawFrame()
      this.pendingHoverPointer = {
        clientX: e.clientX,
        clientY: e.clientY,
      };
      this.scheduleRedraw();
    }
  }

  private onPointerUp(e: PointerEvent, ctrl: ScatterController): void {
    if (this.containerDom === undefined) {
      return;
    }
    try {
      this.containerDom.releasePointerCapture(e.pointerId);
    } catch {
      // Ignore if not captured
    }

    if (this.drag !== undefined) {
      if (this.drag.isShift && this.drag.hasMoved && ctrl.view !== undefined) {
        const {plotWidth, plotHeight} = this.getPlotArea();

        const xMinPx =
          Math.min(this.drag.startX, this.drag.currentX) - MARGIN_LEFT;
        const xMaxPx =
          Math.max(this.drag.startX, this.drag.currentX) - MARGIN_LEFT;
        const yMinPx =
          Math.min(this.drag.startY, this.drag.currentY) - MARGIN_TOP;
        const yMaxPx =
          Math.max(this.drag.startY, this.drag.currentY) - MARGIN_TOP;

        const x0 = screenToDataX(xMinPx, ctrl.view, plotWidth);
        const x1 = screenToDataX(xMaxPx, ctrl.view, plotWidth);
        const y1 = screenToDataY(yMinPx, ctrl.view, plotHeight);
        const y0 = screenToDataY(yMaxPx, ctrl.view, plotHeight);

        ctrl.setBrush({
          xMin: Math.min(x0, x1),
          xMax: Math.max(x0, x1),
          yMin: Math.min(y0, y1),
          yMax: Math.max(y0, y1),
        });
      } else if (!this.drag.hasMoved) {
        // Plain click pins point
        ctrl.pin(ctrl.hover);
      }
      this.drag = undefined;
      this.scheduleRedraw();
    }
  }

  // Document-level keydown handler. Shortcuts apply while the pointer is over
  // the plot or the plot has keyboard focus, and never while typing.
  private onDocumentKeyDown(e: KeyboardEvent): void {
    const ctrl = this.currentController;
    if (ctrl === undefined || this.containerDom === undefined) {
      return;
    }
    if (e.defaultPrevented || e.ctrlKey || e.metaKey || e.altKey) {
      return;
    }
    const target = e.target;
    if (
      target instanceof HTMLElement &&
      (target.isContentEditable ||
        target instanceof HTMLInputElement ||
        target instanceof HTMLTextAreaElement ||
        target instanceof HTMLSelectElement)
    ) {
      return;
    }
    const focused = this.containerDom.contains(document.activeElement);
    if (!this.pointerInside && !focused) {
      return;
    }
    if (this.onKeyDown(e, ctrl)) {
      e.preventDefault();
      e.stopPropagation();
    }
  }

  // Keyboard navigation, mirroring the timeline: W/S (and +/-) zoom at the
  // mouse cursor, A/D pan horizontally, arrow keys pan in all directions.
  // Returns true if the key was handled.
  private onKeyDown(e: KeyboardEvent, ctrl: ScatterController): boolean {
    if (e.key === 'Escape') {
      ctrl.setBrush(undefined);
      ctrl.pin(undefined);
      this.scheduleRedraw();
      return true;
    }
    if (ctrl.view === undefined || ctrl.dataBounds === undefined) {
      return false;
    }

    const anchor = this.pointerInside ? this.lastPointerPx : undefined;
    if (e.code === KeyMapping.KEY_ZOOM_IN || e.key === '+' || e.key === '=') {
      this.zoomAt(ctrl, KEY_ZOOM_FACTOR, KEY_ZOOM_FACTOR, anchor, true);
      return true;
    }
    if (e.code === KeyMapping.KEY_ZOOM_OUT || e.key === '-' || e.key === '_') {
      this.zoomAt(ctrl, 1 / KEY_ZOOM_FACTOR, 1 / KEY_ZOOM_FACTOR, anchor, true);
      return true;
    }

    let dx = 0;
    let dy = 0;
    if (e.code === KeyMapping.KEY_PAN_LEFT || e.key === 'ArrowLeft') {
      dx = -KEY_PAN_FRACTION;
    } else if (e.code === KeyMapping.KEY_PAN_RIGHT || e.key === 'ArrowRight') {
      dx = KEY_PAN_FRACTION;
    } else if (e.key === 'ArrowUp') {
      dy = KEY_PAN_FRACTION;
    } else if (e.key === 'ArrowDown') {
      dy = -KEY_PAN_FRACTION;
    } else {
      return false;
    }
    this.cancelZoomAnimation();
    const view = ctrl.view;
    ctrl.setView(
      panView(view, dx * (view.x1 - view.x0), dy * (view.y1 - view.y0)),
    );
    this.scheduleRedraw();
    return true;
  }

  private renderTooltip(ctrl: ScatterController, hover: PointRef): m.Children {
    const xVal = hover.points.x[hover.index];
    const yVal = hover.points.y[hover.index];
    const colorVal = hover.points.color[hover.index];
    const hoverDetails = ctrl.hoverDetails;

    let colorDisplay: string | undefined;
    if (ctrl.colorMode.kind === 'numeric') {
      colorDisplay = Number.isNaN(colorVal) ? 'NULL' : formatValue(colorVal);
    } else if (ctrl.colorMode.kind === 'categorical') {
      const catIdx = Math.round(colorVal);
      if (catIdx === -1) {
        colorDisplay = 'NULL';
      } else if (catIdx < ctrl.colorMode.categories.length) {
        colorDisplay = ctrl.colorMode.categories[catIdx];
      } else {
        const colorCol = ctrl.colorMode.column;
        const colorCell = hoverDetails?.cells.find((c) => c.name === colorCol);
        colorDisplay =
          colorCell !== undefined && colorCell.value !== null
            ? String(colorCell.value)
            : 'Other';
      }
    }

    let tooltipTitle: string | undefined;
    if (hoverDetails !== undefined) {
      const nameCell = hoverDetails.cells.find((c) => c.name === 'name');
      const idCell = hoverDetails.cells.find((c) => c.name === 'id');
      if (nameCell !== undefined && nameCell.value !== null) {
        tooltipTitle = String(nameCell.value);
      } else if (idCell !== undefined && idCell.value !== null) {
        tooltipTitle = `id: ${idCell.value}`;
      }
    }

    const ptCount = hover.points.weight?.[hover.index];

    return m(
      CursorTooltip,
      {
        className: 'pf-scatter-plot-view__tooltip',
      },
      tooltipTitle !== undefined &&
        m('.pf-scatter-plot-view__tooltip-title', tooltipTitle),
      m(
        '.pf-scatter-plot-view__tooltip-grid',
        m('.pf-scatter-plot-view__tooltip-key', ctrl.plot?.x ?? 'x'),
        m('.pf-scatter-plot-view__tooltip-val', formatValue(xVal)),
        m('.pf-scatter-plot-view__tooltip-key', ctrl.plot?.y ?? 'y'),
        m('.pf-scatter-plot-view__tooltip-val', formatValue(yVal)),
        colorDisplay !== undefined &&
          ctrl.colorMode.kind !== 'none' && [
            m('.pf-scatter-plot-view__tooltip-key', ctrl.colorMode.column),
            m('.pf-scatter-plot-view__tooltip-val', colorDisplay),
          ],
        ptCount !== undefined &&
          ptCount > 1 && [
            m('.pf-scatter-plot-view__tooltip-key', 'points'),
            m(
              '.pf-scatter-plot-view__tooltip-val',
              ptCount.toLocaleString('en-US'),
            ),
          ],
      ),
    );
  }

  private renderLegendOverlay(ctrl: ScatterController): m.Children {
    const colorMode = ctrl.colorMode;
    if (colorMode.kind === 'none') {
      return null;
    }

    const theme = this.getTheme();
    const encoding =
      this.cachedEncoding ??
      buildColorEncoding(colorMode, theme, ctrl.hiddenCategories);

    if (this.legendCollapsed) {
      return m(
        '.pf-scatter-legend.pf-scatter-legend--collapsed',
        {
          onpointerdown: (e: PointerEvent) => e.stopPropagation(),
          onwheel: (e: WheelEvent) => e.stopPropagation(),
        },
        m(Button, {
          icon: 'palette',
          label: `Legend: ${colorMode.column}`,
          compact: true,
          variant: ButtonVariant.Outlined,
          rightIcon: Icons.ExpandDown,
          onclick: (e: MouseEvent) => {
            e.stopPropagation();
            this.legendCollapsed = false;
            m.redraw();
          },
        }),
      );
    }

    const entries =
      colorMode.kind === 'categorical'
        ? legendEntries(colorMode, encoding)
        : [];
    const allIndices = entries.map((entry) => entry.index);
    const hiddenCount = ctrl.hiddenCategories.size;

    return m(
      '.pf-scatter-legend',
      {
        onpointerdown: (e: PointerEvent) => e.stopPropagation(),
        onwheel: (e: WheelEvent) => e.stopPropagation(),
        // Prevent the plot's double-click "reset view" from firing when
        // double-clicking legend entries.
        ondblclick: (e: MouseEvent) => e.stopPropagation(),
      },
      m(
        '.pf-scatter-legend__header',
        m('.pf-scatter-legend__title', `Legend (${colorMode.column})`),
        hiddenCount > 0 &&
          m(Button, {
            label: 'Show all',
            compact: true,
            variant: ButtonVariant.Minimal,
            title: `Show all categories (${hiddenCount} hidden)`,
            onclick: (e: MouseEvent) => {
              e.stopPropagation();
              ctrl.showAllCategories();
            },
          }),
        m(Button, {
          icon: Icons.ExpandUp,
          compact: true,
          variant: ButtonVariant.Minimal,
          title: 'Collapse legend',
          onclick: (e: MouseEvent) => {
            e.stopPropagation();
            this.legendCollapsed = true;
            m.redraw();
          },
        }),
      ),
      colorMode.kind === 'categorical' &&
        m(
          '.pf-scatter-legend__list',
          entries.map((entry) => {
            const isHidden = ctrl.hiddenCategories.has(entry.index);
            return m(
              '.pf-scatter-legend__item',
              {
                key: entry.index,
                className: isHidden ? 'pf-scatter-legend__item--hidden' : '',
                title:
                  `${entry.label}: ${entry.count.toLocaleString('en-US')}` +
                  ' points\nClick to hide/show, double-click to show only this',
                onclick: (e: MouseEvent) => {
                  e.stopPropagation();
                  // The first click of a double-click toggles; the second
                  // isolates. isolateCategory() is defined so that the net
                  // effect of the pair is "show only this" (or, when this was
                  // already the only visible entry, "show all").
                  if (e.detail === 2) {
                    ctrl.isolateCategory(entry.index, allIndices);
                  } else if (e.detail <= 1) {
                    ctrl.toggleCategory(entry.index);
                  }
                },
              },
              m('.pf-scatter-legend__swatch', {
                style: {backgroundColor: entry.css},
              }),
              m('.pf-scatter-legend__label', entry.label),
              m(
                '.pf-scatter-legend__count',
                entry.count.toLocaleString('en-US'),
              ),
            );
          }),
        ),
      colorMode.kind === 'numeric' &&
        m(
          '.pf-scatter-legend__gradient-container',
          m('.pf-scatter-legend__gradient-bar', {
            style: {background: gradientCss()},
          }),
          m(
            '.pf-scatter-legend__gradient-labels',
            m('span', colorMode.min.toLocaleString()),
            m('span', colorMode.max.toLocaleString()),
          ),
        ),
    );
  }

  view({attrs}: m.CVnode<ScatterPlotViewAttrs>) {
    this.currentController = attrs.controller;
    this.currentPointStyle = attrs.pointStyle ?? DEFAULT_STYLE;

    const {plotWidth, plotHeight} = this.getPlotArea();
    this.currentController?.setPlotSize(
      plotWidth,
      plotHeight,
      this.currentPointStyle.sizePx,
    );

    const hover = attrs.controller.hover;

    return m(
      '.pf-scatter-plot-view',
      {
        tabIndex: 0,
        onwheel: (e: WheelEvent) => this.onWheel(e, attrs.controller),
        onpointerdown: (e: PointerEvent) =>
          this.onPointerDown(e, attrs.controller),
        onpointermove: (e: PointerEvent) =>
          this.onPointerMove(e, attrs.controller),
        onpointerup: (e: PointerEvent) => this.onPointerUp(e, attrs.controller),
        onpointerleave: () => {
          this.pointerInside = false;
          this.lastPointerPx = undefined;
          this.pendingHoverPointer = undefined;
          attrs.controller.setHover(undefined);
          this.scheduleRedraw();
        },
        ondblclick: () => attrs.controller.resetView(),
      },
      m(
        '.pf-scatter-plot-view__canvas-wrapper',
        m('canvas.pf-scatter-plot-view__canvas', {
          style: {
            left: `${MARGIN_LEFT}px`,
            top: `${MARGIN_TOP}px`,
            width: `${plotWidth}px`,
            height: `${plotHeight}px`,
          },
          oncreate: (vnode: m.VnodeDOM) => {
            this.plotCanvas = vnode.dom as HTMLCanvasElement;
            this.initRenderer(this.plotCanvas);
            this.scheduleRedraw();
          },
        }),
      ),
      m('canvas.pf-scatter-plot-view__overlay', {
        style: {
          left: '0px',
          top: '0px',
          width: `${this.containerWidth}px`,
          height: `${this.containerHeight}px`,
        },
        oncreate: (vnode: m.VnodeDOM) => {
          this.overlayCanvas = vnode.dom as HTMLCanvasElement;
          this.overlayCanvas.width = Math.max(
            1,
            Math.round(this.containerWidth * this.dpr),
          );
          this.overlayCanvas.height = Math.max(
            1,
            Math.round(this.containerHeight * this.dpr),
          );
          this.scheduleRedraw();
        },
        onupdate: (vnode: m.VnodeDOM) => {
          this.overlayCanvas = vnode.dom as HTMLCanvasElement;
        },
      }),
      this.renderLegendOverlay(attrs.controller),
      hover !== undefined ? this.renderTooltip(attrs.controller, hover) : null,
    );
  }
}
