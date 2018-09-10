// Copyright (C) 2018 The Android Open Source Project
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

import * as m from 'mithril';

import {assertExists} from '../base/logging';

import {globals} from './globals';
import {Panel} from './panel';

/**
 * If the panel container scrolls, the backing canvas height is
 * SCROLLING_CANVAS_OVERDRAW_FACTOR * parent container height.
 */
const SCROLLING_CANVAS_OVERDRAW_FACTOR = 2;

function getCanvasOverdrawHeightPerSide(vnode: PanelContainerVnode) {
  const overdrawHeight =
      (vnode.state.canvasOverdrawFactor - 1) * vnode.state.parentHeight;
  return overdrawHeight / 2;
}

function updateDimensionsFromDom(vnodeDom: PanelContainerVnodeDom) {
  // Get height fron the parent element.
  const rect = vnodeDom.dom.parentElement!.getBoundingClientRect();
  vnodeDom.state.parentWidth = rect.width;
  vnodeDom.state.parentHeight = rect.height;
  const dpr = window.devicePixelRatio;
  const ctx = assertExists(vnodeDom.state.ctx);
  ctx.canvas.width = vnodeDom.state.parentWidth * dpr;
  ctx.canvas.height =
      vnodeDom.state.parentHeight * vnodeDom.state.canvasOverdrawFactor * dpr;
  ctx.scale(dpr, dpr);
}

function panelIsOnCanvas(
    panelYBoundsOnCanvas: {start: number, end: number}, canvasHeight: number) {
  return panelYBoundsOnCanvas.end > 0 &&
      panelYBoundsOnCanvas.start < canvasHeight;
}


function renderPanelCanvas(
    ctx: CanvasRenderingContext2D,
    width: number,
    yStartOnCanvas: number,
    panel: Panel) {
  ctx.save();
  ctx.translate(0, yStartOnCanvas);
  const clipRect = new Path2D();
  clipRect.rect(0, 0, width, panel.getHeight());
  ctx.clip(clipRect);

  panel.renderCanvas(ctx);

  ctx.restore();
}

function redrawAllPanelCavases(vnode: PanelContainerVnode) {
  const state = vnode.state;
  if (!state.ctx) return;
  const canvasHeight = state.parentHeight * state.canvasOverdrawFactor;
  state.ctx.clearRect(0, 0, state.parentWidth, canvasHeight);
  const canvasYStart = state.scrollTop - getCanvasOverdrawHeightPerSide(vnode);

  let panelYStart = 0;
  for (const panel of vnode.attrs.panels) {
    const yStartOnCanvas = panelYStart - canvasYStart;
    const panelHeight = panel.getHeight();
    const panelYBoundsOnCanvas = {
      start: yStartOnCanvas,
      end: yStartOnCanvas + panelHeight,
    };
    if (!panelIsOnCanvas(panelYBoundsOnCanvas, canvasHeight)) {
      panelYStart += panelHeight;
      continue;
    }

    renderPanelCanvas(state.ctx, state.parentWidth, yStartOnCanvas, panel);
    panelYStart += panelHeight;
  }
}

function repositionCanvas(vnodeDom: PanelContainerVnodeDom) {
  const canvas =
      assertExists(vnodeDom.dom.querySelector('canvas.main-canvas')) as
      HTMLElement;
  const canvasYStart =
      vnodeDom.state.scrollTop - getCanvasOverdrawHeightPerSide(vnodeDom);
  canvas.style.transform = `translateY(${canvasYStart}px)`;
}

const PanelComponent = {
  view({attrs}) {
    return m('.panel', {
      style: {height: `${attrs.panel.getHeight()}px`},
    });
  },

  oncreate({dom, attrs}) {
    attrs.panel.updateDom(dom as HTMLElement);
  },

  onupdate({dom, attrs}) {
    attrs.panel.updateDom(dom as HTMLElement);
  }

} as m.Component<{panel: Panel}>;

interface PanelContainerState {
  parentWidth: number;
  parentHeight: number;
  scrollTop: number;
  canvasOverdrawFactor: number;
  ctx: CanvasRenderingContext2D|null;
  panels: Panel[];

  // We store these functions so we can remove them.
  onResize: () => void;
  canvasRedrawer: () => void;
  parentOnScroll: () => void;
}

interface PanelContainerAttrs {
  panels: Panel[];
  doesScroll: boolean;
}

// Vnode contains state + attrs. VnodeDom contains state + attrs + dom.
type PanelContainerVnode = m.Vnode<PanelContainerAttrs, PanelContainerState>;
type PanelContainerVnodeDom =
    m.VnodeDOM<PanelContainerAttrs, PanelContainerState>;

export const PanelContainer = {
  oninit(vnode: PanelContainerVnode) {
    // These values are updated with proper values in oncreate.
    this.parentWidth = 0;
    this.parentHeight = 0;
    this.scrollTop = 0;
    this.canvasOverdrawFactor =
        vnode.attrs.doesScroll ? SCROLLING_CANVAS_OVERDRAW_FACTOR : 1;
    this.ctx = null;
    this.canvasRedrawer = () => redrawAllPanelCavases(vnode);
    this.panels = [];
    globals.rafScheduler.addRedrawCallback(this.canvasRedrawer);
  },

  oncreate(vnodeDom: PanelContainerVnodeDom) {
    // Save the canvas context in the state.
    const canvas =
        vnodeDom.dom.querySelector('.main-canvas') as HTMLCanvasElement;
    const ctx = canvas.getContext('2d');
    if (!ctx) {
      throw Error('Cannot create canvas context');
    }
    this.ctx = ctx;

    // Calling m.redraw during a lifecycle method results in undefined behavior.
    // Use setTimeout to do it asyncronously at the end of the current redraw.
    setTimeout(() => {
      updateDimensionsFromDom(vnodeDom);
      globals.rafScheduler.scheduleFullRedraw();
    });

    // Save the resize handler in the state so we can remove it later.
    // TODO: Encapsulate resize handling better.
    this.onResize = () => {
      updateDimensionsFromDom(vnodeDom);
      globals.rafScheduler.scheduleFullRedraw();
    };

    // Once ResizeObservers are out, we can stop accessing the window here.
    window.addEventListener('resize', this.onResize);

    if (vnodeDom.attrs.doesScroll) {
      this.parentOnScroll = () => {
        vnodeDom.state.scrollTop = vnodeDom.dom.parentElement!.scrollTop;
        repositionCanvas(vnodeDom);
        globals.rafScheduler.scheduleRedraw();
      };
      vnodeDom.dom.parentElement!.addEventListener(
          'scroll', this.parentOnScroll, {passive: true});
    }
  },

  onremove({attrs, dom}) {
    window.removeEventListener('resize', this.onResize);
    globals.rafScheduler.removeRedrawCallback(this.canvasRedrawer);
    if (attrs.doesScroll) {
      dom.parentElement!.removeEventListener('scroll', this.parentOnScroll);
    }
  },

  view({attrs}) {
    const totalHeight =
        attrs.panels.reduce((sum, panel) => sum + panel.getHeight(), 0);
    const canvasHeight = this.parentHeight * this.canvasOverdrawFactor;

    // In the scrolling case, since the canvas is overdrawn and continuously
    // repositioned, we need the canvas to be in a div with overflow hidden and
    // height equaling the total height of the content to prevent scrolling
    // height from growing.
    return m(
        '.scroll-limiter',
        {
          style: {
            height: `${totalHeight}px`,
          }
        },
        m('canvas.main-canvas', {
          style: {
            height: `${canvasHeight}px`,
          }
        }),
        attrs.panels.map(panel => m(PanelComponent, {panel, key: panel.id})));
  },

  onupdate(vnodeDom: PanelContainerVnodeDom) {
    repositionCanvas(vnodeDom);
  }
} as m.Component<PanelContainerAttrs, PanelContainerState>;
