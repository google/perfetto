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

import m from 'mithril';
import {DisposableStack} from '../base/disposable_stack';
import {findRef, toHTMLElement} from '../base/dom_utils';
import {Rect2D, Size2D} from '../base/geom';
import {assertExists} from '../base/logging';
import {VirtualCanvas} from '../base/virtual_canvas';
import {HTMLAttrs} from './common';

const CANVAS_CONTAINER_REF = 'canvas-container';
const CANVAS_OVERDRAW_PX = 300;
const CANVAS_TOLERANCE_PX = 100;

export interface VirtualOverlayCanvasDrawContext {
  // Canvas rendering context.
  readonly ctx: CanvasRenderingContext2D;

  // The size of the virtual canvas element.
  readonly virtualCanvasSize: Size2D;

  // The rect of the actual canvas W.R.T to the virtual canvas element.
  readonly canvasRect: Rect2D;
}

export type Overflow = 'hidden' | 'visible' | 'auto';

export interface VirtualOverlayCanvasAttrs extends HTMLAttrs {
  // Additional class names applied to the root element.
  readonly className?: string;

  // Overflow rules for the horizontal axis.
  readonly overflowX?: Overflow;

  // Overflow rules for the vertical axis.
  readonly overflowY?: Overflow;

  // Called when the canvas needs to be repainted due to a layout shift or
  // or resize.
  onCanvasRedraw?(ctx: VirtualOverlayCanvasDrawContext): void;

  // When true the canvas will not be redrawn on mithril update cycles. Disable
  // this if you want to manage the canvas redraw cycle yourself (i.e. possibly
  // using raf.addCanvasRedrawCallback()) and want to avoid double redraws.
  // Default: false.
  readonly disableCanvasRedrawOnMithrilUpdates?: boolean;

  // Called when the canvas is mounted. The passed redrawCanvas() function can
  // be called to redraw the canvas synchronously at any time. Any returned
  // disposable will be disposed of when the component is removed.
  onMount?(redrawCanvas: () => void): Disposable | void;

  // Override styles from base interface, only allowing object type styles
  // rather than strings.
  style?: Partial<CSSStyleDeclaration>;
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
export class VirtualOverlayCanvas
  implements m.ClassComponent<VirtualOverlayCanvasAttrs>
{
  readonly trash = new DisposableStack();
  private ctx?: CanvasRenderingContext2D;
  private virtualCanvas?: VirtualCanvas;
  private attrs?: VirtualOverlayCanvasAttrs;

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
        // Put canvas container after content so it appears on top. An actual
        // canvas element will be created inside here by the
        // VirtualCanvasHelper.
        m('.pf-virtual-overlay-canvas__canvas-container', {
          ref: CANVAS_CONTAINER_REF,
        }),
      ),
    );
  }

  oncreate({attrs, dom}: m.CVnodeDOM<VirtualOverlayCanvasAttrs>) {
    const canvasContainerElement = toHTMLElement(
      assertExists(findRef(dom, CANVAS_CONTAINER_REF)),
    );
    const {overflowX = 'visible', overflowY = 'visible'} = attrs;

    // Create the virtual canvas inside the canvas container element. We assume
    // the scrolling container is the root level element of this component so we
    // can just use `dom`.
    const virtualCanvas = new VirtualCanvas(canvasContainerElement, dom, {
      overdrawPx: CANVAS_OVERDRAW_PX,
      tolerancePx: CANVAS_TOLERANCE_PX,
      overdrawAxes: getScrollAxesFromOverflow(overflowX, overflowY),
    });
    this.trash.use(virtualCanvas);
    this.virtualCanvas = virtualCanvas;

    // Create the canvas rendering context
    this.ctx = assertExists(virtualCanvas.canvasElement.getContext('2d'));

    // When the container resizes, we might need to resize the canvas. This can
    // be slow so we don't want to do it every render cycle. VirtualCanvas will
    // tell us when we need to do this.
    virtualCanvas.setCanvasResizeListener((canvas, width, height) => {
      const dpr = window.devicePixelRatio;
      canvas.width = width * dpr;
      canvas.height = height * dpr;
    });

    // Whenever the canvas changes size or moves around (e.g. when scrolling),
    // we'll need to trigger a re-render to keep canvas content aligned with the
    // DOM elements underneath.
    virtualCanvas.setLayoutShiftListener(() => {
      this.redrawCanvas();
    });

    const disposable = attrs.onMount?.(this.redrawCanvas.bind(this));
    disposable && this.trash.use(disposable);

    !attrs.disableCanvasRedrawOnMithrilUpdates && this.redrawCanvas();
  }

  onupdate({attrs}: m.CVnodeDOM<VirtualOverlayCanvasAttrs>) {
    !attrs.disableCanvasRedrawOnMithrilUpdates && this.redrawCanvas();
  }

  onremove() {
    this.trash.dispose();
  }

  private redrawCanvas() {
    const ctx = assertExists(this.ctx);
    const virtualCanvas = assertExists(this.virtualCanvas);

    // Reset & clear canvas
    ctx.resetTransform();
    ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);

    // Adjust scaling according pixel ratio. This makes sure the canvas remains
    // sharp on high DPI screens.
    const dpr = window.devicePixelRatio;
    ctx.scale(dpr, dpr);

    // Align canvas rendering offset with the canvas container, not the actual
    // canvas. This means we can ignore the fact that we are using a virtual
    // canvas and just render assuming (0, 0) is at the top left of the canvas
    // container.
    ctx.translate(
      -virtualCanvas.canvasRect.left,
      -virtualCanvas.canvasRect.top,
    );

    assertExists(this.attrs).onCanvasRedraw?.({
      ctx,
      virtualCanvasSize: virtualCanvas.size,
      canvasRect: virtualCanvas.canvasRect,
    });
  }
}
