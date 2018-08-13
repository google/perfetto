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

import {FlameGraphPanel} from './flame_graph_panel';
import {globals} from './globals';
import {OverviewTimelinePanel} from './overview_timeline_panel';
import {Panel} from './panel';
import {TimeAxisPanel} from './time_axis_panel';
import {TrackPanel} from './track_panel';

/**
 * The backing canvas height is CANVAS_OVERDRAW_FACTOR * visible height.
 */
const CANVAS_OVERDRAW_FACTOR = 2;

type CanvasScrollingContainerVnode =
    m.VnodeDOM<{}, ScrollingPanelContainerState>;

function getCanvasOverdrawHeightPerSide(visibleHeight: number) {
  const overdrawHeight = (CANVAS_OVERDRAW_FACTOR - 1) * visibleHeight;
  return overdrawHeight / 2;
}

function updateDimensionsFromDom(vnode: CanvasScrollingContainerVnode) {
  const rect = vnode.dom.getBoundingClientRect();
  vnode.state.domWidth = rect.width;
  vnode.state.domHeight = rect.height;
  const dpr = window.devicePixelRatio;
  const ctx = assertExists(vnode.state.ctx);
  ctx.canvas.width = vnode.state.domWidth * dpr;
  ctx.canvas.height = vnode.state.domHeight * CANVAS_OVERDRAW_FACTOR * dpr;
  ctx.scale(dpr, dpr);
}

/**
 * Stores a panel, and associated metadata.
 */
interface PanelAttrs {
  height: number;
  panel: Panel;
  key: string;
}

const PanelComponent = {
  view({attrs}) {
    return m('.panel', {
      style: {
        height: `${attrs.panelAttrs.height}px`,
        width: '100%',
        position: 'absolute',
        top: `${attrs.yStart}px`,
      },
    });
  },

  oncreate({dom, attrs}) {
    attrs.panelAttrs.panel.updateDom(dom as HTMLElement);
  },

  onupdate({dom, attrs}) {
    attrs.panelAttrs.panel.updateDom(dom as HTMLElement);
    globals.rafScheduler.scheduleOneRedraw();
  }
} as m.Component<{panelAttrs: PanelAttrs, yStart: number}>;

function panelIsOnCanvas(
    panelYBoundsOnCanvas: {start: number, end: number}, canvasHeight: number) {
  return panelYBoundsOnCanvas.end > 0 &&
      panelYBoundsOnCanvas.start < canvasHeight;
}


function renderPanelCanvas(
    ctx: CanvasRenderingContext2D,
    width: number,
    yStartOnCanvas: number,
    panelAttrs: PanelAttrs) {
  ctx.save();
  ctx.translate(0, yStartOnCanvas);
  const clipRect = new Path2D();
  clipRect.rect(0, 0, width, panelAttrs.height);
  ctx.clip(clipRect);

  panelAttrs.panel.renderCanvas(ctx);

  ctx.restore();
}

function redrawAllPanelCavases(state: ScrollingPanelContainerState) {
  if (!state.ctx) return;
  const canvasHeight = state.domHeight * CANVAS_OVERDRAW_FACTOR;
  state.ctx.clearRect(0, 0, state.domWidth, canvasHeight);
  const canvasYStart =
      state.scrollTop - getCanvasOverdrawHeightPerSide(state.domHeight);

  let panelYStart = 0;
  for (const key of state.panelDisplayOrder) {
    const panelAttrs = assertExists(state.keyToPanelAttrs.get(key));
    const yStartOnCanvas = panelYStart - canvasYStart;
    const panelYBoundsOnCanvas = {
      start: yStartOnCanvas,
      end: yStartOnCanvas + panelAttrs.height
    };
    if (!panelIsOnCanvas(panelYBoundsOnCanvas, canvasHeight)) {
      panelYStart += panelAttrs.height;
      continue;
    }

    renderPanelCanvas(state.ctx, state.domWidth, yStartOnCanvas, panelAttrs);
    panelYStart += panelAttrs.height;
  }
}

interface ScrollingPanelContainerState {
  domWidth: number;
  domHeight: number;
  scrollTop: number;
  ctx: CanvasRenderingContext2D|null;
  keyToPanelAttrs: Map<string, PanelAttrs>;
  panelDisplayOrder: string[];

  // We store these functions so we can remove them.
  onResize: () => void;
  canvasRedrawer: () => void;
}

export const ScrollingPanelContainer = {
  oninit({state}) {
    // These values are updated with proper values in oncreate.
    this.domWidth = 0;
    this.domHeight = 0;
    this.scrollTop = 0;
    this.ctx = null;
    this.keyToPanelAttrs = new Map<string, PanelAttrs>();
    this.canvasRedrawer = () => redrawAllPanelCavases(state);
    globals.rafScheduler.addRedrawCallback(this.canvasRedrawer);
  },

  oncreate(vnode) {
    // Save the canvas context in the state.
    const canvas = vnode.dom.querySelector('.main-canvas') as HTMLCanvasElement;
    const ctx = canvas.getContext('2d');
    if (!ctx) {
      throw Error('Cannot create canvas context');
    }
    this.ctx = ctx;

    // updateDimensionsFromDom calls m.redraw, but calling m.redraw during a
    // lifecycle method results in undefined behavior. Use setTimeout to do it
    // asyncronously at the end of the current redraw.
    setTimeout(() => {
      updateDimensionsFromDom(vnode);
      m.redraw();
    });

    // Save the resize handler in the state so we can remove it later.
    // TODO: Encapsulate resize handling better.
    this.onResize = () => {
      updateDimensionsFromDom(vnode);
      m.redraw();
    };

    // Once ResizeObservers are out, we can stop accessing the window here.
    window.addEventListener('resize', this.onResize);

    vnode.dom.addEventListener('scroll', () => {
      vnode.state.scrollTop = vnode.dom.scrollTop;
      m.redraw();
    }, {passive: true});
  },

  onremove() {
    window.removeEventListener('resize', this.onResize);
    globals.rafScheduler.removeRedrawCallback(this.canvasRedrawer);
  },

  view() {
    console.log('ScrollingPanelContainer redraw');
    // TODO: Handle panel deletion.
    // Create all the track panels if they don't already exist.
    for (const id of globals.state.displayedTrackIds) {
      const trackState = globals.state.tracks[id];
      // Makeshift name mangling.
      let panelAttrs = this.keyToPanelAttrs.get('track-' + id);
      if (panelAttrs === undefined) {
        const trackPanel = new TrackPanel(trackState);
        panelAttrs = {
          panel: trackPanel,
          height: trackPanel.getHeight(),
          key: id,
        };
        this.keyToPanelAttrs.set('track-' + id, panelAttrs);
      }
    }

    // Ordered list of panel to display, by key. Store this in state so canvas
    // can use it.
    this.panelDisplayOrder =
        globals.state.displayedTrackIds.map(id => 'track-' + id);

    if (this.keyToPanelAttrs.get('timeaxis') === undefined) {
      const panel = new TimeAxisPanel();
      const spec = {
        panel,
        height: panel.getHeight(),
        key: 'timeaxis',
      };
      this.keyToPanelAttrs.set(spec.key, spec);
    }
    this.panelDisplayOrder.unshift('timeaxis');

    if (this.keyToPanelAttrs.get('overview') === undefined) {
      const panel = new OverviewTimelinePanel();
      const spec = {
        panel,
        height: panel.getHeight(),
        key: 'overview',
      };
      this.keyToPanelAttrs.set(spec.key, spec);
    }
    this.panelDisplayOrder.unshift('overview');

    // Show a fake flame graph if there is at least one track.
    if (globals.state.displayedTrackIds.length > 0) {
      if (!this.keyToPanelAttrs.has('flamegraph')) {
        const panel = new FlameGraphPanel();
        const flameGraphPanelStruct = {
          panel,
          height: panel.getHeight(),
          key: 'flamegraph',
        };
        this.keyToPanelAttrs.set('flamegraph', flameGraphPanelStruct);
      }
      this.panelDisplayOrder.push('flamegraph');
    }

    const panelComponents: m.Children[] = [];
    let yStart = 0;
    for (const key of this.panelDisplayOrder) {
      const panelAttrs = assertExists(this.keyToPanelAttrs.get(key));
      panelComponents.push(m(PanelComponent, {panelAttrs, yStart, key}));
      yStart += panelAttrs.height;
    }

    let totalContentHeight = 0;
    for (const panelAttrs of this.keyToPanelAttrs.values()) {
      totalContentHeight += panelAttrs.height;
    }

    const canvasYStart =
        this.scrollTop - getCanvasOverdrawHeightPerSide(this.domHeight);
    const canvasHeight = this.domHeight * CANVAS_OVERDRAW_FACTOR;

    return m(
        '.scrolling-panel-container',
        // Since the canvas is overdrawn and continuously repositioned, we need
        // the canvas to be in a div with overflow hidden and height equaling
        // the height of the content to prevent scrolling height from growing.
        m('.scroll-limiter',
          {
            style: {
              height: `${totalContentHeight}px`,
              overflow: 'hidden',
              position: 'absolute',
              top: '0px',
              width: '100%',
            }
          },
          m('canvas.main-canvas', {
            style: {
              height: `${canvasHeight}px`,
              top: `${canvasYStart}px`,
              width: '100%',
              position: 'absolute',
            }
          }),
          ...panelComponents));
  },
} as m.Component<{}, ScrollingPanelContainerState>;
