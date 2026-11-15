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
 * VirtualCanvas - A Mithril Component for Virtual Canvas Rendering
 *
 * This module provides a Mithril component that acts as a scrolling container
 * for tall and/or wide content. It overlays a floating canvas on top of its
 * content rendered inside it, which stays in the viewport of scrolling
 * container element as the user scrolls, allowing for rendering of large-scale
 * visualizations which would be too large for a normal HTML canvas element.
 *
 * Key Features:
 * - Supports horizontal, vertical, or both axes scrolling, moving the canvas
 *   while the user scrolls to keep it in the viewport.
 * - Automatically handles canvas resizing using resize observers, including
 *   scaling for high DPI displays.
 * - Calls a callback whenever the canvas needs to be redrawn.
 */

import './virtual_overlay_canvas.scss';
import m from 'mithril';
import {DisposableStack} from '../base/disposable_stack';
import {bindEventListener, findRef, toHTMLElement} from '../base/dom_utils';
import type {Rect2D, Size2D} from '../base/geom';
import {ensureExists} from '../base/assert';
import {VirtualCanvas} from '../base/virtual_canvas';
import {WebGLRenderer} from '../base/gl/webgl_renderer';
import {Canvas2DRenderer} from '../base/canvas2d_renderer';
import type {Renderer} from '../base/renderer';
import type {HTMLAttrs} from './common';

// DO NOT SUBMIT: moire test pattern.
// When enabled, every VirtualOverlayCanvas draws a moire test pattern over the
// top of its normal contents. Widgets cannot depend on feature flags directly,
// so this is set at startup from a feature flag registered in the frontend.
let moireTestPatternEnabled = false;

export function setMoireTestPatternEnabled(enabled: boolean) {
  moireTestPatternEnabled = enabled;
}

const CANVAS_CONTAINER_REF = 'canvas-container';
const CANVAS_OVERDRAW_PX = 300;
const CANVAS_TOLERANCE_PX = 100;

export interface VirtualOverlayCanvasDrawContext {
  // Canvas rendering context.
  readonly ctx: CanvasRenderingContext2D;

  // The virtual canvas DOM element.
  readonly dom: Element;

  // The size of the virtual canvas element.
  readonly virtualCanvasSize: Size2D;

  // The rect of the actual canvas W.R.T to the virtual canvas element.
  // Includes overdraw area for smooth scrolling.
  readonly canvasRect: Rect2D;

  // The rect of the visible viewport W.R.T to the virtual canvas element.
  // Does not include overdraw area.
  readonly viewportRect: Rect2D;

  // Renderer for drawing primitives. This is either a WebGLRenderer (when
  // WebGL is enabled and available) or a Canvas2DRenderer fallback.
  // The renderer is reused across frames - do not store references to it.
  readonly renderer: Renderer;
}

export type Overflow = 'hidden' | 'visible' | 'auto';

// Imperative handle for controlling a VirtualOverlayCanvas. Handed to the
// parent via the `onMount` callback.
export interface VirtualOverlayCanvasApi {
  // Request a redraw of the canvas. The canvas is not redrawn synchronously,
  // but later in the current frame, after layout. Multiple requests in the
  // same frame result in a single redraw.
  redrawCanvas(): void;

  // Set the scroll position of the scrolling container and return the applied
  // position. Either axis may be omitted. The browser clamps to the valid range.
  scrollTo(opts: {x?: number; y?: number}): {x: number; y: number};
}

export interface VirtualOverlayCanvasAttrs extends HTMLAttrs {
  // Additional class names applied to the root element.
  readonly className?: string;

  // Overflow rules for the horizontal axis.
  readonly overflowX?: Overflow;

  // Overflow rules for the vertical axis.
  readonly overflowY?: Overflow;

  // Called when the canvas needs to be repainted due to a layout shift or
  // or resize.
  readonly onCanvasRedraw?: (ctx: VirtualOverlayCanvasDrawContext) => void;

  // When true the canvas will not be redrawn on mithril update cycles. Disable
  // this if you want to manage the canvas redraw cycle yourself (i.e. possibly
  // using raf.addCanvasRedrawCallback()) and want to avoid double redraws.
  // Default: false.
  readonly disableCanvasRedrawOnMithrilUpdates?: boolean;

  // Controls whether the canvas is redrawn when the virtual canvas element
  // (whose size is passed to onCanvasRedraw as virtualCanvasSize) changes size
  // in the inline (horizontal) and/or block (vertical) direction. Redraws
  // caused by the floating canvas itself moving or resizing are unaffected.
  // Default: 'both'.
  readonly redrawOnResize?: 'none' | 'inline' | 'block' | 'both';

  // Called when the canvas is mounted. The passed api object exposes
  // imperative methods for controlling the canvas. Any returned disposable
  // will be disposed of when the component is removed.
  readonly onMount?: (api: VirtualOverlayCanvasApi) => Disposable | void;

  // Override styles from base interface, only allowing object type styles
  // rather than strings.
  readonly style?: Partial<CSSStyleDeclaration>;

  // Enable a second canvas for WebGL rendering. When enabled, webglCanvas and
  // webglCtx will be provided in the draw context.
  // Default: false.
  readonly enableWebGL?: boolean;
}

function getScrollAxesFromOverflow(x: Overflow, y: Overflow) {
  if (x === 'auto' && y === 'auto') {
    return 'both';
  } else if (x === 'auto') {
    return 'x';
  } else if (y === 'auto') {
    return 'y';
  } else {
    //
    return 'none';
  }
}

// This mithril component acts as scrolling container for tall and/or wide
// content. Adds a virtually scrolling canvas over the top of any child elements
// rendered inside it.
export class VirtualOverlayCanvas implements m.ClassComponent<VirtualOverlayCanvasAttrs> {
  readonly trash = new DisposableStack();
  private ctx?: CanvasRenderingContext2D;
  private virtualCanvas?: VirtualCanvas;
  private attrs?: VirtualOverlayCanvasAttrs;
  private webglCanvas?: HTMLCanvasElement;
  private webglRenderer?: WebGLRenderer;
  private dom?: Element;
  private moirePattern?: ImageData; // DO NOT SUBMIT: moire test pattern.
  private requestRedraw?: () => void;

  view({attrs, children}: m.CVnode<VirtualOverlayCanvasAttrs>) {
    this.attrs = attrs;
    const {
      overflowX = 'visible',
      overflowY = 'visible',
      style = {},
      ...rest
    } = attrs;

    return m(
      '.pf-virtual-overlay-canvas', // The scrolling container
      {
        className: attrs.className,
        style: {
          ...style,
          overflowX,
          overflowY,
        },
        ...rest,
      },
      m(
        '.pf-virtual-overlay-canvas__content', // Container for scrolling element, used for sizing the canvas
        children,
        // Put canvas container after content so it appears on top. The actual
        // canvas element(s) are created inside here in oncreate().
        m('.pf-virtual-overlay-canvas__canvas-container', {
          ref: CANVAS_CONTAINER_REF,
        }),
      ),
    );
  }

  oncreate({attrs, dom}: m.CVnodeDOM<VirtualOverlayCanvasAttrs>) {
    this.dom = dom;
    const canvasContainerElement = toHTMLElement(
      ensureExists(findRef(dom, CANVAS_CONTAINER_REF)),
    );
    const {overflowX = 'visible', overflowY = 'visible'} = attrs;

    // Ensures the canvases don't change the size of the container element.
    canvasContainerElement.style.overflow = 'hidden';

    // Create the floating canvas inside the canvas container element.
    const canvasEl = document.createElement('canvas');
    canvasEl.style.position = 'absolute';
    canvasContainerElement.appendChild(canvasEl);
    this.trash.defer(() => {
      canvasContainerElement.removeChild(canvasEl);
    });

    // Create the canvas rendering context
    this.ctx = ensureExists(canvasEl.getContext('2d'));
    const ctx = this.ctx;

    // Create WebGL canvas if enabled
    if (attrs.enableWebGL) {
      this.webglCanvas = document.createElement('canvas');
      this.webglCanvas.style.position = 'absolute';
      this.webglCanvas.style.pointerEvents = 'none';
      // Insert before the 2D canvas so WebGL renders underneath
      canvasContainerElement.insertBefore(this.webglCanvas, canvasEl);
      this.trash.defer(() => {
        if (this.webglCanvas?.parentElement) {
          this.webglCanvas.parentElement.removeChild(this.webglCanvas);
        }
      });

      const webglCtx = this.webglCanvas.getContext('webgl2', {
        alpha: true,
        premultipliedAlpha: true,
        antialias: true,
      });
      if (webglCtx) {
        this.webglRenderer = new WebGLRenderer(this.ctx, webglCtx);
        this.trash.use(
          bindEventListener(
            this.webglCanvas,
            'webglcontextlost',
            (e: Event) => {
              // Set the webgl renderer to undefined means that the canvas
              // renderer will just be used instead.
              this.webglRenderer = undefined;

              // Calling preventDefault here allows tells the browser to send us
              // a 'webglcontextrestored' event when context is restored.
              e.preventDefault();
            },
          ),
        );
        this.trash.use(
          bindEventListener(this.webglCanvas, 'webglcontextrestored', () => {
            // Recreate the webgl renderer to continue rendering with WebGL.
            // WebGL state such as buffers and textures shall be recrated lazily
            // on first render.
            this.webglRenderer = new WebGLRenderer(ctx, webglCtx);
          }),
        );
      }
    }

    // VirtualCanvas works out where the floating canvas should be and how big
    // it should be (in CSS pixels) and tells us whenever that changes. We
    // assume the scrolling container is the root level element of this
    // component so we can just use `dom`.
    //
    // This must be created BEFORE the canvas observer below. Resize observers
    // are called in creation order, so VirtualCanvas's internal observer runs
    // first. If it changes the rect, the requestRedraw() call below replaces
    // any canvas entry that was already due in that pass with a fresh one, so
    // we still only draw once in that frame, with the new rect.
    const virtualCanvas = new VirtualCanvas(
      canvasContainerElement,
      dom,
      (rect) => {
        // Apply the new rect to the canvas element(s). No redraw here - the
        // canvas observer resizes the draw buffers (if required) and redraws
        // later in this frame.
        for (const el of [canvasEl, this.webglCanvas]) {
          if (el === undefined) continue;
          el.style.width = `${rect.width}px`;
          el.style.height = `${rect.height}px`;
          el.style.transform = `translate(${rect.left}px, ${rect.top}px)`;
        }
        requestRedraw();
      },
      {
        overdrawPx: CANVAS_OVERDRAW_PX,
        tolerancePx: CANVAS_TOLERANCE_PX,
        overdrawAxes: getScrollAxesFromOverflow(overflowX, overflowY),
      },
    );
    this.trash.use(virtualCanvas);
    this.virtualCanvas = virtualCanvas;

    // A single observer watches both the canvas element (in device pixels, for
    // sizing the draw buffer) and the canvas container element (in CSS pixels,
    // for redrawOnResize).
    //
    // The canvas entry is the only place that redraws the canvas. Anything that
    // invalidates the canvas (layout changes, mithril updates, explicit redraw
    // requests) calls requestRedraw() which forces a canvas entry to be
    // delivered later in the same frame. This way we redraw at most once per
    // frame, after layout and after the draw buffer is resized.
    let containerInlineSize: number | undefined;
    let containerBlockSize: number | undefined;
    const observer = new ResizeObserver((entries) => {
      let canvasEntry: ResizeObserverEntry | undefined;
      let containerInvalidated = false;

      for (const entry of entries) {
        if (entry.target === canvasEl) {
          canvasEntry = entry;
        } else if (entry.target === canvasContainerElement) {
          const {inlineSize, blockSize} = entry.contentBoxSize[0]!;
          const inlineChanged = inlineSize !== containerInlineSize;
          const blockChanged = blockSize !== containerBlockSize;
          containerInlineSize = inlineSize;
          containerBlockSize = blockSize;

          const redrawOnResize = this.attrs?.redrawOnResize ?? 'none';
          const onInline =
            redrawOnResize === 'inline' || redrawOnResize === 'both';
          const onBlock =
            redrawOnResize === 'block' || redrawOnResize === 'both';
          if ((inlineChanged && onInline) || (blockChanged && onBlock)) {
            containerInvalidated = true;
          }
        }
      }

      if (canvasEntry) {
        const width = canvasEntry.devicePixelContentBoxSize[0]!.inlineSize;
        const height = canvasEntry.devicePixelContentBoxSize[0]!.blockSize;

        // Only touch the draw buffer if its size actually changed - assigning
        // to width/height clears the canvas, and this entry may have been
        // forced by requestRedraw() without the size changing.
        if (canvasEl.width !== width || canvasEl.height !== height) {
          console.log(`Canvas draw buffer resized: ${width}x${height}px`);

          // Resize the regular canvas buffer
          canvasEl.width = width;
          canvasEl.height = height;

          // Resize the WebGL canvas buffer to match. It always has the same
          // CSS size and position as the regular canvas, so it covers the same
          // device pixels.
          if (this.webglCanvas) {
            this.webglCanvas.width = width;
            this.webglCanvas.height = height;
            this.webglRenderer?.gl.viewport(0, 0, width, height);
          }
        }

        // Redraw synchronoously.
        console.log('Redrawing...');
        this.redrawCanvas();
      } else if (containerInvalidated) {
        console.log('Container resized - scheduling a redraw');
        requestRedraw();
      }
    });

    // Re-observing an element creates a fresh observation, which is always
    // delivered once (provided the element has a non-zero size). The canvas is
    // deeper in the DOM than the elements whose layout changes call this, so
    // the entry is delivered in the same frame.
    const canvasObserverOpts = {box: 'device-pixel-content-box'} as const;
    const requestRedraw = () => {
      observer.unobserve(canvasEl);
      observer.observe(canvasEl, canvasObserverOpts);
    };
    this.requestRedraw = requestRedraw;

    observer.observe(canvasEl, canvasObserverOpts);
    observer.observe(canvasContainerElement);
    this.trash.defer(() => {
      observer.disconnect();
    });

    const scrollEl = toHTMLElement(dom);
    const api: VirtualOverlayCanvasApi = {
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
    const disposable = attrs.onMount?.(api);
    disposable && this.trash.use(disposable);

    // No need to draw here: the first canvas entry is delivered later in this
    // frame and draws the canvas.
  }

  onupdate({attrs}: m.CVnodeDOM<VirtualOverlayCanvasAttrs>) {
    !attrs.disableCanvasRedrawOnMithrilUpdates && this.requestRedraw?.();
  }

  onremove() {
    this.trash.dispose();
  }

  private redrawCanvas() {
    const ctx = ensureExists(this.ctx);
    const virtualCanvas = ensureExists(this.virtualCanvas);
    const attrs = ensureExists(this.attrs);
    const containerElement = ensureExists(this.dom);
    const canvasSize = virtualCanvas.size;

    // If the canavs size is 0, just don't render anything. This either means
    // the canavs element is hidden (has no layout) or it genuinely is 0. Either
    // way - there's nothing to be gained from rendering to it.
    if (canvasSize.height <= 0 || canvasSize.width <= 0) {
      return;
    }

    // Create the appropriate renderer: WebGLRenderer if available, otherwise
    // Canvas2DRenderer as fallback.
    const renderer: Renderer = this.webglRenderer ?? new Canvas2DRenderer(ctx);

    // Reset & clear canvas
    renderer.resetTransform();
    renderer.clear();

    // Offse by the virtual canvas position and scale by device pixel ratio
    const dpr = window.devicePixelRatio;
    using _transform = renderer.pushTransform({
      scaleX: dpr,
      scaleY: dpr,
    });
    using _scale = renderer.pushTransform({
      offsetX: -virtualCanvas.canvasRect.left,
      offsetY: -virtualCanvas.canvasRect.top,
    });

    // Call the user-provided draw callback to render into the canvas
    attrs.onCanvasRedraw?.({
      dom: containerElement,
      ctx,
      virtualCanvasSize: virtualCanvas.size,
      canvasRect: virtualCanvas.canvasRect,
      viewportRect: virtualCanvas.viewportRect,
      renderer,
    });

    // Also flush WebGL draw calls (this is distinct from the flush above).
    this.webglRenderer?.gl.flush();

    // DO NOT SUBMIT: moire test pattern.
    if (moireTestPatternEnabled) {
      this.drawMoireTestPattern(ctx);
    }
  }

  // DO NOT SUBMIT: moire test pattern.
  //
  // Overwrites the whole canvas with stripes that are exactly one draw buffer
  // pixel wide, written straight into the buffer (putImageData ignores the
  // current transform):
  // - Left 62% of the buffer: vertical R/G/B stripes (colour = x % 3).
  // - Right 38% of the buffer: horizontal R/G/B stripes (colour = y % 3).
  //
  // If the buffer reaches the screen 1:1, every device pixel is a pure primary.
  // If the browser has to scale the buffer to fit the canvas element, the
  // stripes blend into each other.
  private drawMoireTestPattern(ctx: CanvasRenderingContext2D) {
    const width = ctx.canvas.width;
    const height = ctx.canvas.height;
    if (width <= 0 || height <= 0) return;

    let pattern = this.moirePattern;
    if (
      pattern === undefined ||
      pattern.width !== width ||
      pattern.height !== height
    ) {
      pattern = ctx.createImageData(width, height);
      const data = pattern.data;
      const split = Math.floor(width * 0.62);
      for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
          const channel = (x < split ? x : y) % 3;
          const i = (y * width + x) * 4;
          data[i + channel] = 255;
          data[i + 3] = 255;
        }
      }
      this.moirePattern = pattern;
    }
    ctx.putImageData(pattern, 0, 0);
  }
}
