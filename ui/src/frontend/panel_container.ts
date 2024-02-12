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

import m from 'mithril';

import {Trash} from '../base/disposable';
import {findRef, getScrollbarWidth} from '../base/dom_utils';
import {assertExists, assertFalse} from '../base/logging';
import {SimpleResizeObserver} from '../base/resize_observer';
import {time} from '../base/time';
import {
  debugNow,
  perfDebug,
  perfDisplay,
  PerfStatsSource,
  RunningStatistics,
  runningStatStr,
} from '../core/perf';
import {raf} from '../core/raf_scheduler';
import {SliceRect} from '../public';

import {
  SELECTION_STROKE_COLOR,
  TOPBAR_HEIGHT,
  TRACK_SHELL_WIDTH,
} from './css_constants';
import {
  FlowEventsRenderer,
  FlowEventsRendererArgs,
} from './flow_events_renderer';
import {globals} from './globals';
import {PanelSize} from './panel';

// If the panel container scrolls, the backing canvas height is
// SCROLLING_CANVAS_OVERDRAW_FACTOR * parent container height.
const SCROLLING_CANVAS_OVERDRAW_FACTOR = 1.2;

export interface Panel {
  kind: 'panel';
  mithril: m.Children;
  selectable: boolean;
  key: string;
  trackKey?: string;
  trackGroupId?: string;
  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize): void;
  getSliceRect?(tStart: time, tDur: time, depth: number): SliceRect|undefined;
}

export interface PanelGroup {
  kind: 'group';
  collapsed: boolean;
  header: Panel;
  childTracks: Panel[];
  trackGroupId: string;
}

export type PanelOrGroup = Panel|PanelGroup;

export interface Attrs {
  panels: PanelOrGroup[];
  doesScroll: boolean;
  kind: 'TRACKS'|'OVERVIEW';
  className?: string;
}

interface PanelInfo {
  id: string;  // Can be == '' for singleton panels.
  panel: Panel;
  height: number;
  width: number;
  x: number;
  y: number;
}

export class PanelContainer implements m.ClassComponent<Attrs>,
                                       PerfStatsSource {
  // These values are updated with proper values in oncreate.
  private parentWidth = 0;
  private parentHeight = 0;
  private scrollTop = 0;
  private panelInfos: PanelInfo[] = [];
  private panelContainerTop = 0;
  private panelContainerHeight = 0;
  private panelByKey = new Map<string, Panel>();
  private totalPanelHeight = 0;
  private canvasHeight = 0;

  private flowEventsRenderer: FlowEventsRenderer;

  private panelPerfStats = new WeakMap<Panel, RunningStatistics>();
  private perfStats = {
    totalPanels: 0,
    panelsOnCanvas: 0,
    renderStats: new RunningStatistics(10),
  };

  // Attrs received in the most recent mithril redraw. We receive a new vnode
  // with new attrs on every redraw, and we cache it here so that resize
  // listeners and canvas redraw callbacks can access it.
  private attrs: Attrs;

  private ctx?: CanvasRenderingContext2D;

  private trash: Trash;

  private readonly SCROLL_LIMITER_REF = 'scroll-limiter';
  private readonly PANELS_REF = 'panels';

  get canvasOverdrawFactor() {
    return this.attrs.doesScroll ? SCROLLING_CANVAS_OVERDRAW_FACTOR : 1;
  }

  getPanelsInRegion(startX: number, endX: number, startY: number, endY: number):
      Panel[] {
    const minX = Math.min(startX, endX);
    const maxX = Math.max(startX, endX);
    const minY = Math.min(startY, endY);
    const maxY = Math.max(startY, endY);
    const panels: Panel[] = [];
    for (let i = 0; i < this.panelInfos.length; i++) {
      const pos = this.panelInfos[i];
      const realPosX = pos.x - TRACK_SHELL_WIDTH;
      if (realPosX + pos.width >= minX && realPosX <= maxX &&
          pos.y + pos.height >= minY && pos.y <= maxY && pos.panel.selectable) {
        panels.push(pos.panel);
      }
    }
    return panels;
  }

  // This finds the tracks covered by the in-progress area selection. When
  // editing areaY is not set, so this will not be used.
  handleAreaSelection() {
    const area = globals.timeline.selectedArea;
    if (area === undefined || globals.timeline.areaY.start === undefined ||
        globals.timeline.areaY.end === undefined ||
        this.panelInfos.length === 0) {
      return;
    }
    // Only get panels from the current panel container if the selection began
    // in this container.
    const panelContainerTop = this.panelInfos[0].y;
    const panelContainerBottom = this.panelInfos[this.panelInfos.length - 1].y +
        this.panelInfos[this.panelInfos.length - 1].height;
    if (globals.timeline.areaY.start + TOPBAR_HEIGHT < panelContainerTop ||
        globals.timeline.areaY.start + TOPBAR_HEIGHT > panelContainerBottom) {
      return;
    }

    const {visibleTimeScale} = globals.timeline;

    // The Y value is given from the top of the pan and zoom region, we want it
    // from the top of the panel container. The parent offset corrects that.
    const panels = this.getPanelsInRegion(
      visibleTimeScale.timeToPx(area.start),
      visibleTimeScale.timeToPx(area.end),
      globals.timeline.areaY.start + TOPBAR_HEIGHT,
      globals.timeline.areaY.end + TOPBAR_HEIGHT);
    // Get the track ids from the panels.
    const tracks = [];
    for (const panel of panels) {
      if (panel.trackKey !== undefined) {
        tracks.push(panel.trackKey);
        continue;
      }
      if (panel.trackGroupId !== undefined) {
        const trackGroup = globals.state.trackGroups[panel.trackGroupId];
        // Only select a track group and all child tracks if it is closed.
        if (trackGroup.collapsed) {
          tracks.push(panel.trackGroupId);
          for (const track of trackGroup.tracks) {
            tracks.push(track);
          }
        }
      }
    }
    globals.timeline.selectArea(area.start, area.end, tracks);
  }

  constructor(vnode: m.CVnode<Attrs>) {
    this.attrs = vnode.attrs;
    this.flowEventsRenderer = new FlowEventsRenderer();
    this.trash = new Trash();

    const onRedraw = () => this.redrawCanvas();
    raf.addRedrawCallback(onRedraw);
    this.trash.addCallback(() => {
      raf.removeRedrawCallback(onRedraw);
    });

    perfDisplay.addContainer(this);
    this.trash.addCallback(() => {
      perfDisplay.removeContainer(this);
    });
  }

  oncreate({dom}: m.CVnodeDOM<Attrs>) {
    // Save the canvas context in the state.
    const canvas = dom.querySelector('.main-canvas') as HTMLCanvasElement;
    const ctx = canvas.getContext('2d');
    if (!ctx) {
      throw Error('Cannot create canvas context');
    }
    this.ctx = ctx;

    this.readParentSizeFromDom(dom);
    this.readPanelHeightsFromDom(dom);

    this.updateCanvasDimensions();
    this.repositionCanvas();

    const scrollLimiter = assertExists(findRef(dom, this.SCROLL_LIMITER_REF));
    this.trash.add(new SimpleResizeObserver(scrollLimiter, () => {
      const parentSizeChanged = this.readParentSizeFromDom(dom);
      if (parentSizeChanged) {
        this.updateCanvasDimensions();
        this.repositionCanvas();
        this.redrawCanvas();
      }
    }));

    // TODO(dproy): Handle change in doesScroll attribute.
    if (this.attrs.doesScroll) {
      const parentOnScroll = () => {
        this.scrollTop = dom.scrollTop;
        this.repositionCanvas();
        raf.scheduleRedraw();
      };
      dom.addEventListener(
        'scroll', parentOnScroll, {passive: true});
      this.trash.addCallback(() => {
        dom.removeEventListener('scroll', parentOnScroll);
      });
    }
  }

  onremove() {
    this.trash.dispose();
  }

  renderPanel(node: Panel, key: string, extraClass = ''): m.Vnode {
    assertFalse(this.panelByKey.has(key));
    this.panelByKey.set(key, node);
    const mithril = node.mithril;

    return m(
      `.panel${extraClass}`,
      {key, 'data-key': key},
      perfDebug() ?
        [mithril, m('.debug-panel-border', {key: 'debug-panel-border'})] :
        mithril);
  }

  // Render a tree of panels into one vnode. Argument `path` is used to build
  // `key` attribute for intermediate tree vnodes: otherwise Mithril internals
  // will complain about keyed and non-keyed vnodes mixed together.
  renderTree(node: PanelOrGroup, path: string): m.Vnode {
    if (node.kind === 'group') {
      return m(
        'div',
        {key: path},
        this.renderPanel(
          node.header, `${path}-header`, node.collapsed ? '' : '.sticky'),
        ...node.childTracks.map(
          (child, index) => this.renderTree(child, `${path}-${index}`)));
    }
    return this.renderPanel(node, assertExists(node.key));
  }

  view({attrs}: m.CVnode<Attrs>) {
    this.attrs = attrs;
    this.panelByKey.clear();
    const children = attrs.panels.map(
      (panel, index) => this.renderTree(panel, `track-tree-${index}`));

    return m('.panel-container', {className: attrs.className},
      m('.panels', {ref: this.PANELS_REF},
        m('.scroll-limiter', {ref: this.SCROLL_LIMITER_REF},
          m('canvas.main-canvas'),
        ),
        children,
      ),
    );
  }

  onupdate({dom}: m.CVnodeDOM<Attrs>) {
    const totalPanelHeightChanged = this.readPanelHeightsFromDom(dom);
    const parentSizeChanged = this.readParentSizeFromDom(dom);
    const canvasSizeShouldChange =
        parentSizeChanged || !this.attrs.doesScroll && totalPanelHeightChanged;
    if (canvasSizeShouldChange) {
      this.updateCanvasDimensions();
      this.repositionCanvas();
      if (this.attrs.kind === 'TRACKS') {
        globals.timeline.updateLocalLimits(
          0, this.parentWidth - TRACK_SHELL_WIDTH);
      }
      this.redrawCanvas();
    }
  }

  private updateCanvasDimensions() {
    this.canvasHeight = Math.floor(
      this.attrs.doesScroll ? this.parentHeight * this.canvasOverdrawFactor :
        this.totalPanelHeight);
    const ctx = assertExists(this.ctx);
    const canvas = assertExists(ctx.canvas);
    canvas.style.height = `${this.canvasHeight}px`;

    // If're we're non-scrolling canvas and the scroll-limiter should always
    // have the same height. Enforce this by explicitly setting the height.
    if (!this.attrs.doesScroll) {
      const scrollLimiter = canvas.parentElement;
      if (scrollLimiter) {
        scrollLimiter.style.height = `${this.canvasHeight}px`;
      }
    }

    const dpr = window.devicePixelRatio;
    ctx.canvas.width = this.parentWidth * dpr;
    ctx.canvas.height = this.canvasHeight * dpr;
    ctx.scale(dpr, dpr);
  }

  private repositionCanvas() {
    const canvas = assertExists(assertExists(this.ctx).canvas);
    const canvasYStart =
        Math.floor(this.scrollTop - this.getCanvasOverdrawHeightPerSide());
    canvas.style.transform = `translateY(${canvasYStart}px)`;
  }

  // Reads dimensions of parent node. Returns true if read dimensions are
  // different from what was cached in the state.
  private readParentSizeFromDom(dom: Element): boolean {
    const oldWidth = this.parentWidth;
    const oldHeight = this.parentHeight;
    const clientRect = dom.getBoundingClientRect();
    // On non-MacOS if there is a solid scroll bar it can cover important
    // pixels, reduce the size of the canvas so it doesn't overlap with
    // the scroll bar.
    this.parentWidth = clientRect.width - getScrollbarWidth();
    this.parentHeight = clientRect.height;
    return this.parentHeight !== oldHeight || this.parentWidth !== oldWidth;
  }

  // Reads dimensions of panels. Returns true if total panel height is different
  // from what was cached in state.
  private readPanelHeightsFromDom(dom: Element): boolean {
    const prevHeight = this.totalPanelHeight;
    this.panelInfos = [];
    this.totalPanelHeight = 0;

    const panels = assertExists(findRef(dom, this.PANELS_REF));
    const domRect = panels.getBoundingClientRect();
    this.panelContainerTop = domRect.y;
    this.panelContainerHeight = domRect.height;

    dom.querySelectorAll('.panel').forEach((panelElement) => {
      const key = assertExists(panelElement.getAttribute('data-key'));
      const panel = assertExists(this.panelByKey.get(key));

      // NOTE: the id can be undefined for singletons like overview timeline.
      const id = panel.trackKey || panel.trackGroupId || '';
      const rect = panelElement.getBoundingClientRect();
      this.panelInfos.push({
        id,
        height: rect.height,
        width: rect.width,
        x: rect.x,
        y: rect.y,
        panel,
      });
      this.totalPanelHeight += rect.height;
    });

    return this.totalPanelHeight !== prevHeight;
  }

  private overlapsCanvas(yStart: number, yEnd: number) {
    return yEnd > 0 && yStart < this.canvasHeight;
  }

  private redrawCanvas() {
    const redrawStart = debugNow();
    if (!this.ctx) return;
    this.ctx.clearRect(0, 0, this.parentWidth, this.canvasHeight);
    const canvasYStart =
        Math.floor(this.scrollTop - this.getCanvasOverdrawHeightPerSide());

    this.handleAreaSelection();

    let panelYStart = 0;
    let totalOnCanvas = 0;
    const flowEventsRendererArgs =
        new FlowEventsRendererArgs(this.parentWidth, this.canvasHeight);
    for (let i = 0; i < this.panelInfos.length; i++) {
      const panel = this.panelInfos[i].panel;
      const panelHeight = this.panelInfos[i].height;
      const yStartOnCanvas = panelYStart - canvasYStart;

      flowEventsRendererArgs.registerPanel(panel, yStartOnCanvas, panelHeight);

      if (!this.overlapsCanvas(yStartOnCanvas, yStartOnCanvas + panelHeight)) {
        panelYStart += panelHeight;
        continue;
      }

      totalOnCanvas++;

      this.ctx.save();
      this.ctx.translate(0, yStartOnCanvas);
      const clipRect = new Path2D();
      const size = {width: this.parentWidth, height: panelHeight};
      clipRect.rect(0, 0, size.width, size.height);
      this.ctx.clip(clipRect);
      const beforeRender = debugNow();
      panel.renderCanvas(this.ctx, size);
      this.updatePanelStats(
        i, panel, debugNow() - beforeRender, this.ctx, size);
      this.ctx.restore();
      panelYStart += panelHeight;
    }

    this.drawTopLayerOnCanvas();
    this.flowEventsRenderer.render(this.ctx, flowEventsRendererArgs);
    // Collect performance as the last thing we do.
    const redrawDur = debugNow() - redrawStart;
    this.updatePerfStats(redrawDur, this.panelInfos.length, totalOnCanvas);
  }

  // The panels each draw on the canvas but some details need to be drawn across
  // the whole canvas rather than per panel.
  private drawTopLayerOnCanvas() {
    if (!this.ctx) return;
    const area = globals.timeline.selectedArea;
    if (area === undefined || globals.timeline.areaY.start === undefined ||
        globals.timeline.areaY.end === undefined) {
      return;
    }
    if (this.panelInfos.length === 0 || area.tracks.length === 0) return;

    // Find the minY and maxY of the selected tracks in this panel container.
    let selectedTracksMinY = this.panelContainerHeight + this.panelContainerTop;
    let selectedTracksMaxY = this.panelContainerTop;
    let trackFromCurrentContainerSelected = false;
    for (let i = 0; i < this.panelInfos.length; i++) {
      if (area.tracks.includes(this.panelInfos[i].id)) {
        trackFromCurrentContainerSelected = true;
        selectedTracksMinY = Math.min(selectedTracksMinY, this.panelInfos[i].y);
        selectedTracksMaxY = Math.max(
          selectedTracksMaxY,
          this.panelInfos[i].y + this.panelInfos[i].height);
      }
    }

    // No box should be drawn if there are no selected tracks in the current
    // container.
    if (!trackFromCurrentContainerSelected) {
      return;
    }

    const {visibleTimeScale} = globals.timeline;
    const startX = visibleTimeScale.timeToPx(area.start);
    const endX = visibleTimeScale.timeToPx(area.end);
    // To align with where to draw on the canvas subtract the first panel Y.
    selectedTracksMinY -= this.panelContainerTop;
    selectedTracksMaxY -= this.panelContainerTop;
    this.ctx.save();
    this.ctx.strokeStyle = SELECTION_STROKE_COLOR;
    this.ctx.lineWidth = 1;
    const canvasYStart =
        Math.floor(this.scrollTop - this.getCanvasOverdrawHeightPerSide());
    this.ctx.translate(TRACK_SHELL_WIDTH, -canvasYStart);
    this.ctx.strokeRect(
      startX,
      selectedTracksMaxY,
      endX - startX,
      selectedTracksMinY - selectedTracksMaxY);
    this.ctx.restore();
  }

  private updatePanelStats(
    panelIndex: number, panel: Panel, renderTime: number,
    ctx: CanvasRenderingContext2D, size: PanelSize) {
    if (!perfDebug()) return;
    let renderStats = this.panelPerfStats.get(panel);
    if (renderStats === undefined) {
      renderStats = new RunningStatistics();
      this.panelPerfStats.set(panel, renderStats);
    }
    renderStats.addValue(renderTime);

    const statW = 300;
    ctx.fillStyle = 'hsl(97, 100%, 96%)';
    ctx.fillRect(size.width - statW, size.height - 20, statW, 20);
    ctx.fillStyle = 'hsla(122, 77%, 22%)';
    const statStr = `Panel ${panelIndex + 1} | ` + runningStatStr(renderStats);
    ctx.fillText(statStr, size.width - statW, size.height - 10);
  }

  private updatePerfStats(
    renderTime: number, totalPanels: number, panelsOnCanvas: number) {
    if (!perfDebug()) return;
    this.perfStats.renderStats.addValue(renderTime);
    this.perfStats.totalPanels = totalPanels;
    this.perfStats.panelsOnCanvas = panelsOnCanvas;
  }

  renderPerfStats() {
    return [
      m('div',
        `${this.perfStats.totalPanels} panels, ` +
            `${this.perfStats.panelsOnCanvas} on canvas.`),
      m('div', runningStatStr(this.perfStats.renderStats)),
    ];
  }

  private getCanvasOverdrawHeightPerSide() {
    const overdrawHeight = (this.canvasOverdrawFactor - 1) * this.parentHeight;
    return overdrawHeight / 2;
  }
}
