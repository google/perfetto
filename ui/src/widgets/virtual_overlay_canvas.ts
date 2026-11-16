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
 * VirtualOverlayCanvas - A Mithril Component for Virtual Canvas Rendering
 *
 * A VirtualCanvasHost with a single 2D canvas: a scrolling container for tall
 * and/or wide content, with a floating canvas over the top of it that stays in
 * the viewport as the user scrolls. Before each redraw, the canvas is cleared
 * and its context transformed, so the draw callback can simply draw in CSS
 * pixels w.r.t. the virtual canvas.
 *
 * Use VirtualCanvasHost directly if you need more than one canvas or a
 * different kind of rendering context.
 */

import m from 'mithril';
import {DisposableStack} from '../base/disposable_stack';
import {ensureExists} from '../base/assert';
import {
  VirtualCanvasHost,
  type VirtualCanvasHostAttrs,
  type VirtualCanvasHostDrawContext,
} from './virtual_canvas_host';

export type {Scroll} from './virtual_canvas_host';

// DO NOT SUBMIT: moire test pattern.
// When enabled, canvases draw a moire test pattern over the top of their
// normal contents (see maybeDrawMoireTestPattern()). Widgets cannot depend on
// feature flags directly, so this is set at startup from a feature flag
// registered in the frontend.
let moireTestPatternEnabled = false;

export function setMoireTestPatternEnabled(enabled: boolean) {
  moireTestPatternEnabled = enabled;
}

export interface VirtualOverlayCanvasDrawContext extends VirtualCanvasHostDrawContext {
  // Canvas rendering context. It has been cleared, and transformed so that
  // drawing happens in CSS pixels w.r.t. the virtual canvas element.
  readonly ctx: CanvasRenderingContext2D;
}

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

// What onMount can return. Both parts are optional, so a plain Disposable (or
// nothing at all) can be returned too.
export interface VirtualOverlayCanvasMountResult {
  // Called whenever the canvas needs to be redrawn, after the onCanvasRedraw
  // attr (if any). Being created in onMount, it can capture anything set up
  // there.
  readonly onCanvasRedraw?: (ctx: VirtualOverlayCanvasDrawContext) => void;

  // Called when the component is removed.
  [Symbol.dispose]?(): void;
}

export interface VirtualOverlayCanvasAttrs extends Omit<
  VirtualCanvasHostAttrs,
  'onCanvasRedraw' | 'onMount'
> {
  // Called when the canvas needs to be repainted due to a layout shift or
  // resize, or a redraw request.
  readonly onCanvasRedraw?: (ctx: VirtualOverlayCanvasDrawContext) => void;

  // Called when the canvas is mounted. The passed api object exposes
  // imperative methods for controlling the canvas. May return a redraw
  // callback and/or a disposable - see VirtualOverlayCanvasMountResult.
  readonly onMount?: (
    api: VirtualOverlayCanvasApi,
  ) => VirtualOverlayCanvasMountResult | void;
}

// This mithril component acts as scrolling container for tall and/or wide
// content. Adds a virtually scrolling canvas over the top of any child elements
// rendered inside it.
export class VirtualOverlayCanvas implements m.ClassComponent<VirtualOverlayCanvasAttrs> {
  private attrs?: VirtualOverlayCanvasAttrs;

  view({attrs, children}: m.CVnode<VirtualOverlayCanvasAttrs>) {
    // Keep hold of the latest attrs: the onCanvasRedraw attr can change on
    // every render.
    this.attrs = attrs;
    const {onMount, onCanvasRedraw: _onCanvasRedraw, ...rest} = attrs;
    return m(
      VirtualCanvasHost,
      {
        ...rest,
        onMount: (api) => {
          const trash = new DisposableStack();

          const canvas = document.createElement('canvas');
          const ctx = ensureExists(canvas.getContext('2d'));
          trash.use(api.addCanvas(canvas));

          const result = onMount?.({
            redrawCanvas: api.redrawCanvas,
            scrollTo: api.scrollTo,
          });
          const dispose = result ? result[Symbol.dispose] : undefined;
          dispose && trash.defer(() => dispose.call(result));

          return {
            onCanvasRedraw: (drawCtx) => {
              // Reset & clear the canvas.
              ctx.resetTransform();
              ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);

              // Scale from CSS pixels to draw buffer pixels and offset by the
              // floating canvas position, so the draw callbacks can draw in
              // CSS pixels w.r.t. the virtual canvas.
              const {deviceScale: scale, canvasRect} = drawCtx;
              ctx.save();
              ctx.setTransform(
                scale.x,
                0,
                0,
                scale.y,
                -canvasRect.left * scale.x,
                -canvasRect.top * scale.y,
              );
              const vocCtx = {...drawCtx, ctx};
              this.attrs?.onCanvasRedraw?.(vocCtx);
              result?.onCanvasRedraw?.(vocCtx);
              ctx.restore();

              maybeDrawMoireTestPattern(ctx);
            },
            [Symbol.dispose]: () => trash.dispose(),
          };
        },
      },
      children,
    );
  }
}

// DO NOT SUBMIT: moire test pattern.
//
// If the moire test pattern is enabled, overwrites the whole canvas with
// stripes that are exactly one draw buffer pixel wide, written straight into
// the buffer (putImageData ignores the current transform):
// - Left 62% of the buffer: vertical R/G/B stripes (colour = x % 3).
// - Right 38% of the buffer: horizontal R/G/B stripes (colour = y % 3).
//
// If the buffer reaches the screen 1:1, every device pixel is a pure primary.
// If the browser has to scale the buffer to fit the canvas element, the
// stripes blend into each other.
const moirePatterns = new WeakMap<
  HTMLCanvasElement | OffscreenCanvas,
  ImageData
>();

export function maybeDrawMoireTestPattern(ctx: CanvasRenderingContext2D) {
  if (!moireTestPatternEnabled) return;

  const width = ctx.canvas.width;
  const height = ctx.canvas.height;
  if (width <= 0 || height <= 0) return;

  let pattern = moirePatterns.get(ctx.canvas);
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
    moirePatterns.set(ctx.canvas, pattern);
  }
  ctx.putImageData(pattern, 0, 0);
}
