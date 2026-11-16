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

/**
 * VirtualCanvasHost - A Mithril Component for Virtual Canvas Rendering
 *
 * This module provides a Mithril component that acts as a scrolling container
 * for tall and/or wide content, and hosts floating canvases over the top of
 * the content rendered inside it. The canvases stay in the viewport of the
 * scrolling container as the user scrolls, allowing for rendering of
 * large-scale visualizations which would be too large for a normal HTML canvas
 * element.
 *
 * Canvases have limits on their maximum size (which is determined by the
 * system). Usually, this limit is fairly large, but can be as small as
 * 4096x4096px on some machines. However, users can only ever see the part of
 * the content that is inside the viewport of the scrolling container. So
 * instead of one huge canvas, we position a small floating canvas over the
 * viewport and move it around as the user scrolls, approximating a canvas of
 * virtually unlimited size.
 *
 * The host doesn't create or draw to any canvases itself. The parent creates
 * them (and any rendering contexts) and hands them over with addCanvas() in
 * onMount. The host positions and sizes them, keeps their draw buffers the
 * exact size of the device pixels they cover, and calls onCanvasRedraw
 * whenever they need to be redrawn - at most once per frame.
 *
 * See VirtualOverlayCanvas for a ready-made wrapper with a single 2D canvas.
 */

import './virtual_canvas_host.scss';
import m from 'mithril';
import {DisposableStack} from '../base/disposable_stack';
import {bindEventListener, findRef, toHTMLElement} from '../base/dom_utils';
import {Rect2D, type Size2D} from '../base/geom';
import {assertDefined, assertIsInstance, ensureExists} from '../base/assert';
import type {HTMLAttrs} from './common';

const CANVAS_CONTAINER_REF = 'canvas-container';
const FLOATING_REF = 'floating';

// Defaults for the overdrawPx and tolerancePx attrs.
const DEFAULT_OVERDRAW_PX = 300;
const DEFAULT_TOLERANCE_PX = 100;

const EMPTY_RECT = new Rect2D({left: 0, top: 0, right: 0, bottom: 0});

export interface VirtualCanvasHostDrawContext {
  // The virtual canvas DOM element.
  readonly dom: Element;

  // The size of the virtual canvas element.
  readonly virtualCanvasSize: Size2D;

  // The rect of the floating canvases W.R.T to the virtual canvas element.
  // Includes overdraw area for smooth scrolling.
  readonly canvasRect: Rect2D;

  // The rect of the visible viewport W.R.T to the virtual canvas element.
  // Does not include overdraw area.
  readonly viewportRect: Rect2D;

  // How many draw buffer pixels there are per CSS pixel, along each axis. Use
  // this rather than window.devicePixelRatio to scale drawing: it is measured
  // from the actual (pixel snapped) draw buffers, so it maps CSS pixels onto
  // them exactly, and can differ slightly between the axes.
  readonly deviceScale: {readonly x: number; readonly y: number};
}

export type Scroll = 'none' | 'x' | 'y' | 'both';

// Imperative handle for controlling a VirtualCanvasHost. Handed to the parent
// via the `onMount` callback.
export interface VirtualCanvasHostApi {
  // Request a redraw of the canvases. They are not redrawn synchronously, but
  // later in the current frame, after layout. Multiple requests in the same
  // frame result in a single redraw.
  redrawCanvas(): void;

  // Set the scroll position of the scrolling container and return the applied
  // position. Either axis may be omitted. The browser clamps to the valid range.
  scrollTo(opts: {x?: number; y?: number}): {x: number; y: number};

  // Add a canvas element, stacked on top of any previously added ones. The host
  // positions it, sizes it and keeps its draw buffer the size of the device
  // pixels it covers, but never touches its contents: the caller owns the
  // canvas and any context on it, and has to clear and draw it itself from
  // onCanvasRedraw. Note that resizing the draw buffer resets the canvas'
  // contents and context state. Disposing the returned handle removes the
  // canvas again.
  addCanvas(canvas: HTMLCanvasElement): Disposable;
}

// What onMount can return. Both parts are optional, so a plain Disposable (or
// nothing at all) can be returned too.
export interface VirtualCanvasHostMountResult {
  // Called whenever the canvases need to be redrawn, after the onCanvasRedraw
  // attr (if any). Being created in onMount, it can capture anything set up
  // there, such as the canvases and their rendering contexts.
  readonly onCanvasRedraw?: (ctx: VirtualCanvasHostDrawContext) => void;

  // Called when the component is removed.
  [Symbol.dispose]?(): void;
}

export interface VirtualCanvasHostAttrs extends HTMLAttrs {
  // Additional class names applied to the root element.
  readonly className?: string;

  // Override styles from base interface, only allowing object type styles
  // rather than strings.
  readonly style?: Partial<CSSStyleDeclaration>;

  // Which axis/axes to scroll & virtualize. Fixed when the component is
  // created.
  readonly scroll?: Scroll;

  // How much extra canvas to draw around the visible viewport, in the axes
  // that scroll. The larger this number, the further we can scroll before the
  // canvases have to be moved and redrawn, but the more there is to draw each
  // time. Fixed when the component is created.
  // Default: 300.
  readonly overdrawPx?: number;

  // How close the viewport can get to the edge of the drawn area before the
  // canvases are moved. If 0, we can get all the way to the edge before moving
  // them. Larger values move them more often, but make it less likely to see
  // blank bits of canvas when scrolling quickly. Clamped to overdrawPx. Fixed
  // when the component is created.
  // Default: 100.
  readonly tolerancePx?: number;

  // When true the canvases will not be redrawn on mithril update cycles.
  // Disable this if you want to manage the redraw cycle yourself (i.e. possibly
  // using raf.addCanvasRedrawCallback()) and want to avoid double redraws.
  // Default: false.
  readonly disableCanvasRedrawOnMithrilUpdates?: boolean;

  // Called when the canvases need to be repainted due to a layout shift or
  // resize, or a redraw request.
  readonly onCanvasRedraw?: (ctx: VirtualCanvasHostDrawContext) => void;

  // Called when the host is mounted. The passed api object exposes imperative
  // methods for controlling the host, including adding canvases. May return a
  // redraw callback and/or a disposable - see VirtualCanvasHostMountResult.
  readonly onMount?: (
    api: VirtualCanvasHostApi,
  ) => VirtualCanvasHostMountResult | void;
}

// This mithril component acts as scrolling container for tall and/or wide
// content. Hosts virtually scrolling canvases over the top of any child
// elements rendered inside it.
export class VirtualCanvasHost implements m.ClassComponent<VirtualCanvasHostAttrs> {
  private readonly trash = new DisposableStack();
  private attrs?: VirtualCanvasHostAttrs;
  private dom?: Element;
  private requestRedraw?: () => void;

  // The redraw callback returned from onMount, if any.
  private mountRedraw?: (ctx: VirtualCanvasHostDrawContext) => void;

  // The canvas container is the "virtual canvas": an element the size of the
  // whole content, which the floating element lives inside.
  private canvasContainerEl?: HTMLElement;

  // The floating element is positioned over the viewport (plus overdraw). The
  // canvases live inside it and fill it, so only this element ever has to be
  // moved and resized.
  private floatingEl?: HTMLElement;
  private scrollAxes: Scroll = 'none';
  private overdrawPx = DEFAULT_OVERDRAW_PX;
  private tolerancePx = DEFAULT_TOLERANCE_PX;

  // Canvases added via the api's addCanvas().
  private readonly canvases = new Set<HTMLCanvasElement>();

  // The size of the canvases' draw buffers, in device pixels.
  private bufferSize: Size2D = {width: 0, height: 0};

  // Draw buffer pixels per CSS pixel along each axis, measured from the
  // floating element's last resize observer entry.
  private deviceScale = {x: 1, y: 1};

  // The rect of the floating element w.r.t. the canvas container, in CSS
  // pixels.
  private canvasRect = EMPTY_RECT;

  // The visible part of the canvas container (the scrolling container's
  // viewport) w.r.t. the canvas container. Does not include overdraw.
  private viewportRect = EMPTY_RECT;

  // The viewport can move around inside these limits without the canvases
  // having to be moved.
  private viewportLimits = EMPTY_RECT;

  view({attrs, children}: m.CVnode<VirtualCanvasHostAttrs>) {
    this.attrs = attrs;
    const {
      scroll = 'none',
      style = {},
      overdrawPx: _overdrawPx,
      tolerancePx: _tolerancePx,
      ...rest
    } = attrs;

    // Scroll in the scrolling axes and clip in the others. If nothing scrolls,
    // leave overflow alone.
    const scrollX = scroll === 'x' || scroll === 'both';
    const scrollY = scroll === 'y' || scroll === 'both';
    const overflow =
      scroll === 'none'
        ? {}
        : {
            overflowX: scrollX ? 'auto' : 'hidden',
            overflowY: scrollY ? 'auto' : 'hidden',
          };

    return m(
      '.pf-virtual-canvas-host', // The scrolling container
      {
        className: attrs.className,
        style: {
          ...style,
          ...overflow,
        },
        ...rest,
      },
      m(
        '.pf-virtual-canvas-host__content', // Container for scrolling element, used for sizing the canvas
        {
          // A block is only as wide as its container, so content that scrolls
          // horizontally would just overflow it. Size it to the content
          // instead, but at least as wide as the viewport.
          style: scrollX ? {width: 'max-content', minWidth: '100%'} : {},
        },
        children,
        // Put canvas container after content so it appears on top. The
        // canvases are added inside the floating element.
        m(
          '.pf-virtual-canvas-host__canvas-container',
          {ref: CANVAS_CONTAINER_REF},
          m('.pf-virtual-canvas-host__floating', {ref: FLOATING_REF}),
        ),
      ),
    );
  }

  oncreate({attrs, dom}: m.CVnodeDOM<VirtualCanvasHostAttrs>) {
    const {
      scroll = 'none',
      overdrawPx = DEFAULT_OVERDRAW_PX,
      tolerancePx = DEFAULT_TOLERANCE_PX,
    } = attrs;

    this.dom = dom;
    this.scrollAxes = scroll;
    this.overdrawPx = Math.max(0, overdrawPx);
    this.tolerancePx = Math.min(Math.max(0, tolerancePx), this.overdrawPx);

    const canvasContainerEl = findRef(dom, CANVAS_CONTAINER_REF);
    assertIsInstance(canvasContainerEl, HTMLElement);
    this.canvasContainerEl = canvasContainerEl;

    const floatingEl = findRef(dom, FLOATING_REF);
    assertIsInstance(floatingEl, HTMLElement);
    this.floatingEl = floatingEl;

    // A single resize observer drives everything. It watches:
    // - The scrolling container (`dom`) and the canvas container: when either
    //   changes size, or the container is scrolled (see onScroll below), work
    //   out where the floating element should be.
    // - The floating element itself, in device pixels: size the canvases' draw
    //   buffers to match exactly and redraw.
    //
    // All the entries for an observer pass arrive in a single callback, so the
    // order is explicit: first update the canvas rect. If it changed, apply it
    // to the floating element and wait for the forced floating element entry
    // in the next pass - it is deeper in the DOM than the elements that
    // triggered this, so that is still delivered in this frame. Otherwise, if
    // the floating element was reported, size the buffers and draw.
    //
    // The floating element entry is the only place that draws. Anything that
    // invalidates the canvases (layout changes, mithril updates, explicit
    // redraw requests) calls requestRedraw(), which forces a floating element
    // entry to be delivered later in the same frame. This way we draw at most
    // once per frame, after layout and after the draw buffers have been
    // resized.
    const observer = new ResizeObserver((entries) => {
      let floatingEntry: ResizeObserverEntry | undefined;
      let layoutChanged = false;

      for (const entry of entries) {
        if (entry.target === floatingEl) {
          floatingEntry = entry;
        } else if (entry.target === canvasContainerEl) {
          layoutChanged = true;
        } else if (entry.target === dom) {
          layoutChanged = true;
        }
      }

      if (layoutChanged && this.updateCanvasRect()) {
        // The canvases have moved and/or changed size, so anything drawn now
        // would be in the wrong place. Draw when the forced floating element
        // entry arrives in the next pass instead.
        requestRedraw();
        return;
      }

      if (floatingEntry) {
        const devicePixelSize = floatingEntry.devicePixelContentBoxSize[0];
        assertDefined(devicePixelSize);

        // Note: Assumes horizontal writing mode.
        const width = devicePixelSize.inlineSize;
        const height = devicePixelSize.blockSize;

        // Work out the scale from CSS pixels to draw buffer pixels, from the
        // same entry so both sizes describe the same layout.
        const cssSize = floatingEntry.contentBoxSize[0];
        assertDefined(cssSize);
        if (cssSize.inlineSize > 0 && cssSize.blockSize > 0) {
          this.deviceScale = {
            x: width / cssSize.inlineSize,
            y: height / cssSize.blockSize,
          };
        }

        // Only touch the draw buffers if their size actually changed -
        // assigning to width/height clears a canvas, and this entry may have
        // been forced by requestRedraw() without the size changing.
        this.bufferSize = {width, height};
        for (const canvas of this.canvases) {
          this.sizeBuffer(canvas);
        }

        // Redraw synchronoously.
        console.log('Redrawing...');
        this.redrawCanvas();
      }
    });

    // Re-observing an element creates a fresh observation, which is always
    // delivered once. The floating element is deeper in the DOM than the
    // elements whose layout changes call this, so the entry is delivered in
    // the same frame.
    const floatingObserverOpts = {box: 'device-pixel-content-box'} as const;
    const requestRedraw = () => {
      observer.unobserve(floatingEl);
      observer.observe(floatingEl, floatingObserverOpts);
    };
    this.requestRedraw = requestRedraw;

    observer.observe(dom);
    observer.observe(canvasContainerEl);
    observer.observe(floatingEl, floatingObserverOpts);
    this.trash.defer(() => {
      observer.disconnect();
    });

    // Scrolling doesn't recompute anything directly. Instead, re-observe the
    // canvas container: this creates a fresh observation which is always
    // delivered once, later in this frame. That way the canvas rect is always
    // worked out in one place - the observer callback - after layout.
    this.trash.use(
      bindEventListener(
        dom,
        'scroll',
        () => {
          console.log('onScroll');
          observer.unobserve(canvasContainerEl);
          observer.observe(canvasContainerEl);
        },
        {passive: true},
      ),
    );

    // Remove any canvases that are still attached when we go away.
    this.trash.defer(() => {
      for (const canvas of this.canvases) canvas.remove();
      this.canvases.clear();
    });

    const scrollEl = toHTMLElement(dom);
    const api: VirtualCanvasHostApi = {
      addCanvas: (canvas) => this.addCanvas(canvas),
      redrawCanvas: () => {
        console.log('Redraw request from raf scheduler');
        requestRedraw();
      },
      scrollTo: ({x, y}) => {
        if (x !== undefined) scrollEl.scrollLeft = x;
        if (y !== undefined) scrollEl.scrollTop = y;
        return {x: scrollEl.scrollLeft, y: scrollEl.scrollTop};
      },
    };

    const result = attrs.onMount?.(api);
    if (result) {
      this.mountRedraw = result.onCanvasRedraw;
      const dispose = result[Symbol.dispose];
      dispose && this.trash.defer(() => dispose.call(result));
    }

    // No need to draw here: the first floating element entry is delivered
    // later in this frame and draws the canvases.
  }

  onupdate({attrs}: m.CVnodeDOM<VirtualCanvasHostAttrs>) {
    !attrs.disableCanvasRedrawOnMithrilUpdates && this.requestRedraw?.();
  }

  onremove() {
    this.trash.dispose();
  }

  // Works out where the floating element should be, and how big, in CSS pixels
  // w.r.t. the canvas container. It is only moved once the viewport gets within
  // tolerancePx of the edge of the drawn area, so we don't have to move and
  // redraw the canvases every time the user scrolls a little.
  //
  // If the rect changed, it is applied to the floating element and this
  // returns true. Measures the DOM, so only call it when layout is clean, i.e.
  // from the resize observer callback.
  private updateCanvasRect(): boolean {
    const scrollContainer = ensureExists(this.dom);
    const target = ensureExists(this.canvasContainerEl);

    // The scrolling container's viewport w.r.t. the canvas container.
    const targetRect = target.getBoundingClientRect();
    const viewport = new Rect2D(scrollContainer.getBoundingClientRect())
      .intersect(targetRect)
      .reframe(targetRect);
    this.viewportRect = viewport;

    const axes = this.scrollAxes;
    const overdrawX = axes === 'x' || axes === 'both';
    const overdrawY = axes === 'y' || axes === 'both';

    // In the scrolling axes, only move the canvases once the viewport leaves
    // the limits.
    let scrollRect = this.canvasRect;
    if (!this.viewportLimits.contains(viewport)) {
      const expandBy = (px: number) => ({
        width: overdrawX ? px : 0,
        height: overdrawY ? px : 0,
      });
      scrollRect = viewport.expand(expandBy(this.overdrawPx));
      this.viewportLimits = viewport.expand(
        expandBy(this.overdrawPx - this.tolerancePx),
      );
    }

    // In the axes that don't scroll, the floating element is sized to 100% of
    // the canvas container in CSS, so it always spans the whole of it.
    const canvasRect = new Rect2D({
      left: overdrawX ? scrollRect.left : 0,
      right: overdrawX ? scrollRect.right : targetRect.width,
      top: overdrawY ? scrollRect.top : 0,
      bottom: overdrawY ? scrollRect.bottom : targetRect.height,
    });

    const prev = this.canvasRect;
    this.canvasRect = canvasRect;
    if (
      prev.left === canvasRect.left &&
      prev.top === canvasRect.top &&
      prev.width === canvasRect.width &&
      prev.height === canvasRect.height
    ) {
      return false;
    }

    // The canvases need to move and/or change size.
    console.log('onLayoutShift');
    const el = ensureExists(this.floatingEl);
    el.style.width = overdrawX ? `${canvasRect.width}px` : '100%';
    el.style.height = overdrawY ? `${canvasRect.height}px` : '100%';
    el.style.transform = `translate(${canvasRect.left}px, ${canvasRect.top}px)`;
    return true;
  }

  // Sizes a canvas' draw buffer to the current buffer size, if it differs.
  private sizeBuffer(canvas: HTMLCanvasElement) {
    const {width, height} = this.bufferSize;
    if (canvas.width !== width || canvas.height !== height) {
      console.log(`Canvas draw buffer resized: ${width}x${height}px`);
      canvas.width = width;
      canvas.height = height;
    }
  }

  private addCanvas(canvas: HTMLCanvasElement): Disposable {
    // Match the other canvases straight away, so a canvas added after the
    // first layout doesn't have to wait for the next one.
    this.sizeBuffer(canvas);
    ensureExists(this.floatingEl).appendChild(canvas);
    this.canvases.add(canvas);

    return {
      [Symbol.dispose]: () => {
        if (this.canvases.delete(canvas)) {
          canvas.remove();
        }
      },
    };
  }

  // The size of the canvas container, aka the size of the virtual canvas.
  private get virtualCanvasSize(): Size2D {
    const target = ensureExists(this.canvasContainerEl);
    return {width: target.clientWidth, height: target.clientHeight};
  }

  private redrawCanvas() {
    const attrs = ensureExists(this.attrs);
    const canvasSize = this.virtualCanvasSize;

    // If the canavs size is 0, just don't render anything. This either means
    // the canavs element is hidden (has no layout) or it genuinely is 0. Either
    // way - there's nothing to be gained from rendering to it.
    if (canvasSize.height <= 0 || canvasSize.width <= 0) {
      return;
    }

    const drawCtx: VirtualCanvasHostDrawContext = {
      dom: ensureExists(this.dom),
      virtualCanvasSize: canvasSize,
      canvasRect: this.canvasRect,
      viewportRect: this.viewportRect,
      deviceScale: this.deviceScale,
    };
    attrs.onCanvasRedraw?.(drawCtx);
    this.mountRedraw?.(drawCtx);
  }
}
