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
import {ensureExists} from '../../../base/assert';
import type {Rect2D} from '../../../base/geom';
import {
  type Scroll,
  VirtualCanvasHost,
  type VirtualCanvasHostDrawContext,
} from '../../../widgets/virtual_canvas_host';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';

const CELL_WIDTH_PX = 100;
const CELL_HEIGHT_PX = 40;

// Content size in the axes that scroll - far too big for a single canvas.
const LONG_CONTENT_PX = 1_000_000;

// Content size in the axes that don't scroll.
const SHORT_CONTENT_PX = 360;

const SCROLL_OPTIONS: Scroll[] = ['none', 'x', 'y', 'both'];

interface VirtualCanvasHostDemoAttrs {
  readonly scroll: Scroll;
  readonly overdrawPx: number;
  readonly tolerancePx: number;
  readonly width: number;
  readonly height: number;
  readonly disableCanvasRedrawOnMithrilUpdates: boolean;
}

// Draws a grid of labelled cells over content which is very long in the axes
// that scroll. The demo creates its own 2D canvas in onMount and hands it to
// the host, which positions and sizes it. onMount also returns the redraw
// callback, which captures the canvas' context - only the cells under the
// canvas are drawn. Below the host, some stats about the floating canvas are
// shown.
class VirtualCanvasHostDemo implements m.ClassComponent<VirtualCanvasHostDemoAttrs> {
  private statsEl?: Element;
  private drawCount = 0;

  view({attrs}: m.CVnode<VirtualCanvasHostDemoAttrs>) {
    const {
      scroll,
      overdrawPx,
      tolerancePx,
      width,
      height,
      disableCanvasRedrawOnMithrilUpdates,
    } = attrs;
    const scrollX = scroll === 'x' || scroll === 'both';
    const scrollY = scroll === 'y' || scroll === 'both';
    const contentWidth = scrollX ? LONG_CONTENT_PX : SHORT_CONTENT_PX;
    const contentHeight = scrollY ? LONG_CONTENT_PX : SHORT_CONTENT_PX;

    return [
      m(
        VirtualCanvasHost,
        {
          style: {
            width: `${width}px`,
            height: `${height}px`,
            border: '1px solid var(--pf-color-border)',
          },
          scroll,
          overdrawPx,
          tolerancePx,
          disableCanvasRedrawOnMithrilUpdates,
          onMount: (api) => {
            const canvas = document.createElement('canvas');
            const ctx = ensureExists(canvas.getContext('2d'));
            const canvasHandle = api.addCanvas(canvas);
            return {
              onCanvasRedraw: (drawCtx) => this.draw(ctx, drawCtx),
              [Symbol.dispose]: () => canvasHandle[Symbol.dispose](),
            };
          },
        },
        m('', {
          style: {width: `${contentWidth}px`, height: `${contentHeight}px`},
        }),
      ),
      // Updated directly from the draw callback rather than through mithril,
      // as a mithril redraw would itself request a canvas redraw.
      m('pre', {
        style: {fontSize: '11px', margin: '8px 0 0 0'},
        oncreate: ({dom}: m.VnodeDOM) => {
          this.statsEl = dom;
        },
      }),
    ];
  }

  private draw(
    ctx: CanvasRenderingContext2D,
    drawCtx: VirtualCanvasHostDrawContext,
  ) {
    const {canvasRect, viewportRect, deviceScale} = drawCtx;
    this.updateStats(ctx, drawCtx);

    // We own the canvas, so we have to clear it ourselves.
    ctx.resetTransform();
    ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);

    // Scale from CSS pixels to draw buffer pixels and offset by the floating
    // canvas position, so we can draw in CSS pixels w.r.t. the whole content.
    ctx.setTransform(
      deviceScale.x,
      0,
      0,
      deviceScale.y,
      -canvasRect.left * deviceScale.x,
      -canvasRect.top * deviceScale.y,
    );

    // Only draw the cells under the floating canvas.
    const firstCol = Math.floor(canvasRect.left / CELL_WIDTH_PX);
    const lastCol = Math.ceil(canvasRect.right / CELL_WIDTH_PX);
    const firstRow = Math.floor(canvasRect.top / CELL_HEIGHT_PX);
    const lastRow = Math.ceil(canvasRect.bottom / CELL_HEIGHT_PX);

    ctx.font = '12px sans-serif';
    ctx.textBaseline = 'middle';
    for (let row = firstRow; row < lastRow; row++) {
      for (let col = firstCol; col < lastCol; col++) {
        const x = col * CELL_WIDTH_PX;
        const y = row * CELL_HEIGHT_PX;
        ctx.fillStyle =
          (row + col) % 2 === 0
            ? 'rgba(100, 150, 255, 0.25)'
            : 'rgba(100, 150, 255, 0.1)';
        ctx.fillRect(x, y, CELL_WIDTH_PX, CELL_HEIGHT_PX);
        ctx.fillStyle = 'gray';
        ctx.fillText(`${col}, ${row}`, x + 6, y + CELL_HEIGHT_PX / 2);
      }
    }

    // Outline the visible viewport (as reported to the draw callback), inset
    // slightly so it can be seen.
    ctx.strokeStyle = 'rgba(255, 100, 0, 0.8)';
    ctx.lineWidth = 2;
    ctx.strokeRect(
      viewportRect.left + 4,
      viewportRect.top + 4,
      Math.max(0, viewportRect.width - 8),
      Math.max(0, viewportRect.height - 8),
    );
  }

  private updateStats(
    ctx: CanvasRenderingContext2D,
    {
      canvasRect,
      viewportRect,
      virtualCanvasSize,
      deviceScale,
    }: VirtualCanvasHostDrawContext,
  ) {
    this.drawCount++;
    if (this.statsEl === undefined) return;
    const rect = (r: Rect2D) =>
      `left ${r.left.toFixed(1)}, top ${r.top.toFixed(1)}, ` +
      `${r.width.toFixed(1)} x ${r.height.toFixed(1)}`;
    const canvas = ctx.canvas;
    this.statsEl.textContent = [
      `draws:          ${this.drawCount}`,
      `virtual canvas: ${virtualCanvasSize.width} x ${virtualCanvasSize.height}`,
      `viewport rect:  ${rect(viewportRect)}`,
      `canvas rect:    ${rect(canvasRect)}`,
      `draw buffer:    ${canvas.width} x ${canvas.height}`,
      `device scale:   ${deviceScale.x.toFixed(4)}, ${deviceScale.y.toFixed(4)}`,
    ].join('\n');
  }
}

export function renderVirtualCanvasHost(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'VirtualCanvasHost'),
      m(
        'p',
        'A scrolling container that hosts floating canvases over its content, ' +
          'keeping them in the viewport as the user scrolls. It does not create ' +
          'any canvases itself - the parent creates them and hands them over in ' +
          'onMount. In the axes that scroll, the content here is ' +
          `${LONG_CONTENT_PX.toLocaleString()}px long; otherwise it is ` +
          `${SHORT_CONTENT_PX}px. The orange outline is the visible viewport, ` +
          'as reported to the draw callback. Watch the canvas rect and draw ' +
          'count while scrolling to see the effect of the overdraw and ' +
          'tolerance.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({scroll, overdrawPx, tolerancePx, ...rest}) => [
        // The scroll axes, overdraw and tolerance are fixed when the host is
        // created, so recreate it whenever they change.
        m(VirtualCanvasHostDemo, {
          key: `${scroll}-${overdrawPx}-${tolerancePx}`,
          scroll: scroll as Scroll,
          overdrawPx,
          tolerancePx,
          ...rest,
        }),
      ],
      initialOpts: {
        scroll: new EnumOption('y', SCROLL_OPTIONS),
        overdrawPx: 300,
        tolerancePx: 100,
        width: 400,
        height: 400,
        disableCanvasRedrawOnMithrilUpdates: false,
      },
    }),
  ];
}
