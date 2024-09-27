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
import {findRef, toHTMLElement} from '../base/dom_utils';
import {assertExists, assertFalse} from '../base/logging';
import {
  PerfStatsSource,
  RunningStatistics,
  debugNow,
  perfDebug,
  perfDisplay,
  runningStatStr,
} from '../core/perf';
import {raf} from '../core/raf_scheduler';
import {SimpleResizeObserver} from '../base/resize_observer';
import {canvasClip} from '../base/canvas_utils';
import {
  SELECTION_STROKE_COLOR,
  TOPBAR_HEIGHT,
  TRACK_SHELL_WIDTH,
} from './css_constants';
import {globals} from './globals';
import {Bounds2D, Size2D, VerticalBounds} from '../base/geom';
import {VirtualCanvas} from './virtual_canvas';
import {DisposableStack} from '../base/disposable_stack';
import {TimeScale} from '../base/time_scale';
import {Optional} from '../base/utils';
import {TrackNode} from '../public/workspace';
import {HTMLAttrs} from '../widgets/common';

const CANVAS_OVERDRAW_PX = 100;

export interface Panel {
  readonly kind: 'panel';
  render(): m.Children;
  readonly selectable: boolean;
  // TODO(stevegolton): Remove this - panel container should know nothing of
  // tracks!
  readonly trackNode?: TrackNode;
  renderCanvas(ctx: CanvasRenderingContext2D, size: Size2D): void;
  getSliceVerticalBounds?(depth: number): Optional<VerticalBounds>;
}

export interface PanelGroup {
  readonly kind: 'group';
  readonly collapsed: boolean;
  readonly header?: Panel;
  readonly topOffsetPx: number;
  readonly sticky: boolean;
  readonly childPanels: PanelOrGroup[];
}

export type PanelOrGroup = Panel | PanelGroup;

export interface PanelContainerAttrs {
  panels: PanelOrGroup[];
  className?: string;
  onPanelStackResize?: (width: number, height: number) => void;

  // Called after all panels have been rendered to the canvas, to give the
  // caller the opportunity to render an overlay on top of the panels.
  renderOverlay?(
    ctx: CanvasRenderingContext2D,
    size: Size2D,
    panels: ReadonlyArray<RenderedPanelInfo>,
  ): void;

  // Called before the panels are rendered
  renderUnderlay?(ctx: CanvasRenderingContext2D, size: Size2D): void;
}

interface PanelInfo {
  trackNode?: TrackNode; // Can be undefined for singleton panels.
  panel: Panel;
  height: number;
  width: number;
  clientX: number;
  clientY: number;
}

export interface RenderedPanelInfo {
  panel: Panel;
  rect: Bounds2D;
}

export class PanelContainer
  implements m.ClassComponent<PanelContainerAttrs>, PerfStatsSource
{
  // These values are updated with proper values in oncreate.
  // Y position of the panel container w.r.t. the client
  private panelContainerTop = 0;
  private panelContainerHeight = 0;

  // Updated every render cycle in the view() hook
  private panelById = new Map<string, Panel>();

  // Updated every render cycle in the oncreate/onupdate hook
  private panelInfos: PanelInfo[] = [];

  private panelPerfStats = new WeakMap<Panel, RunningStatistics>();
  private perfStats = {
    totalPanels: 0,
    panelsOnCanvas: 0,
    renderStats: new RunningStatistics(10),
  };

  private ctx?: CanvasRenderingContext2D;

  private readonly trash = new DisposableStack();

  private readonly OVERLAY_REF = 'overlay';
  private readonly PANEL_STACK_REF = 'panel-stack';

  getPanelsInRegion(
    startX: number,
    endX: number,
    startY: number,
    endY: number,
  ): Panel[] {
    const minX = Math.min(startX, endX);
    const maxX = Math.max(startX, endX);
    const minY = Math.min(startY, endY);
    const maxY = Math.max(startY, endY);
    const panels: Panel[] = [];
    for (let i = 0; i < this.panelInfos.length; i++) {
      const pos = this.panelInfos[i];
      const realPosX = pos.clientX - TRACK_SHELL_WIDTH;
      if (
        realPosX + pos.width >= minX &&
        realPosX <= maxX &&
        pos.clientY + pos.height >= minY &&
        pos.clientY <= maxY &&
        pos.panel.selectable
      ) {
        panels.push(pos.panel);
      }
    }
    return panels;
  }

  // This finds the tracks covered by the in-progress area selection. When
  // editing areaY is not set, so this will not be used.
  handleAreaSelection() {
    const area = globals.timeline.selectedArea;
    if (
      area === undefined ||
      globals.timeline.areaY.start === undefined ||
      globals.timeline.areaY.end === undefined ||
      this.panelInfos.length === 0
    ) {
      return;
    }
    // Only get panels from the current panel container if the selection began
    // in this container.
    const panelContainerTop = this.panelInfos[0].clientY;
    const panelContainerBottom =
      this.panelInfos[this.panelInfos.length - 1].clientY +
      this.panelInfos[this.panelInfos.length - 1].height;
    if (
      globals.timeline.areaY.start + TOPBAR_HEIGHT < panelContainerTop ||
      globals.timeline.areaY.start + TOPBAR_HEIGHT > panelContainerBottom
    ) {
      return;
    }

    // TODO(stevegolton): We shouldn't know anything about visible time scale
    // right now, that's a job for our parent, but we can put one together so we
    // don't have to refactor this entire bit right now...

    const visibleTimeScale = new TimeScale(globals.timeline.visibleWindow, {
      left: 0,
      right: this.virtualCanvas!.size.width - TRACK_SHELL_WIDTH,
    });

    // The Y value is given from the top of the pan and zoom region, we want it
    // from the top of the panel container. The parent offset corrects that.
    const panels = this.getPanelsInRegion(
      visibleTimeScale.timeToPx(area.start),
      visibleTimeScale.timeToPx(area.end),
      globals.timeline.areaY.start + TOPBAR_HEIGHT,
      globals.timeline.areaY.end + TOPBAR_HEIGHT,
    );
    // Get the track ids from the panels.
    const trackUris: string[] = [];
    for (const panel of panels) {
      if (panel.trackNode) {
        if (panel.trackNode.hasChildren) {
          const groupNode = panel.trackNode;
          // Select a track group and all child tracks if it is collapsed
          if (groupNode.collapsed) {
            for (const track of groupNode.flatTracks) {
              track.uri && trackUris.push(track.uri);
            }
          }
        } else {
          panel.trackNode.uri && trackUris.push(panel.trackNode.uri);
        }
      }
    }
    globals.timeline.selectArea(area.start, area.end, trackUris);
  }

  constructor({attrs}: m.CVnode<PanelContainerAttrs>) {
    const onRedraw = () => this.renderCanvas(attrs);
    raf.addRedrawCallback(onRedraw);
    this.trash.defer(() => {
      raf.removeRedrawCallback(onRedraw);
    });

    perfDisplay.addContainer(this);
    this.trash.defer(() => {
      perfDisplay.removeContainer(this);
    });
  }

  private virtualCanvas?: VirtualCanvas;

  oncreate(vnode: m.CVnodeDOM<PanelContainerAttrs>) {
    const {dom, attrs} = vnode;

    const overlayElement = toHTMLElement(
      assertExists(findRef(dom, this.OVERLAY_REF)),
    );

    const virtualCanvas = new VirtualCanvas(overlayElement, dom, {
      overdrawPx: CANVAS_OVERDRAW_PX,
    });
    this.trash.use(virtualCanvas);
    this.virtualCanvas = virtualCanvas;

    const ctx = virtualCanvas.canvasElement.getContext('2d');
    if (!ctx) {
      throw Error('Cannot create canvas context');
    }
    this.ctx = ctx;

    virtualCanvas.setCanvasResizeListener((canvas, width, height) => {
      const dpr = window.devicePixelRatio;
      canvas.width = width * dpr;
      canvas.height = height * dpr;
    });

    virtualCanvas.setLayoutShiftListener(() => {
      this.renderCanvas(vnode.attrs);
    });

    this.onupdate(vnode);

    const panelStackElement = toHTMLElement(
      assertExists(findRef(dom, this.PANEL_STACK_REF)),
    );

    // Listen for when the panel stack changes size
    this.trash.use(
      new SimpleResizeObserver(panelStackElement, () => {
        attrs.onPanelStackResize?.(
          panelStackElement.clientWidth,
          panelStackElement.clientHeight,
        );
      }),
    );
  }

  onremove() {
    this.trash.dispose();
  }

  renderPanel(node: Panel, panelId: string, htmlAttrs?: HTMLAttrs): m.Vnode {
    assertFalse(this.panelById.has(panelId));
    this.panelById.set(panelId, node);
    return m(
      `.pf-panel`,
      {...htmlAttrs, 'data-panel-id': panelId},
      node.render(),
    );
  }

  // Render a tree of panels into one vnode. Argument `path` is used to build
  // `key` attribute for intermediate tree vnodes: otherwise Mithril internals
  // will complain about keyed and non-keyed vnodes mixed together.
  renderTree(node: PanelOrGroup, panelId: string): m.Vnode {
    if (node.kind === 'group') {
      const style = {
        position: 'sticky',
        top: `${node.topOffsetPx}px`,
        zIndex: `${2000 - node.topOffsetPx}`,
      };
      return m(
        'div.pf-panel-group',
        node.header &&
          this.renderPanel(node.header, `${panelId}-header`, {
            style: !node.collapsed && node.sticky ? style : {},
          }),
        ...node.childPanels.map((child, index) =>
          this.renderTree(child, `${panelId}-${index}`),
        ),
      );
    }
    return this.renderPanel(node, panelId);
  }

  view({attrs}: m.CVnode<PanelContainerAttrs>) {
    this.panelById.clear();
    const children = attrs.panels.map((panel, index) =>
      this.renderTree(panel, `${index}`),
    );

    return m(
      '.pf-panel-container',
      {className: attrs.className},
      m(
        '.pf-panel-stack',
        {ref: this.PANEL_STACK_REF},
        m('.pf-overlay', {ref: this.OVERLAY_REF}),
        children,
      ),
    );
  }

  onupdate({dom}: m.CVnodeDOM<PanelContainerAttrs>) {
    this.readPanelRectsFromDom(dom);
  }

  private readPanelRectsFromDom(dom: Element): void {
    this.panelInfos = [];

    const panels = assertExists(findRef(dom, this.PANEL_STACK_REF));
    const domRect = panels.getBoundingClientRect();
    this.panelContainerTop = domRect.y;
    this.panelContainerHeight = domRect.height;

    dom.querySelectorAll('.pf-panel').forEach((panelElement) => {
      const panelHTMLElement = toHTMLElement(panelElement);
      const panelId = assertExists(panelHTMLElement.dataset.panelId);
      const panel = assertExists(this.panelById.get(panelId));

      // NOTE: the id can be undefined for singletons like overview timeline.
      const rect = panelElement.getBoundingClientRect();
      this.panelInfos.push({
        trackNode: panel.trackNode,
        height: rect.height,
        width: rect.width,
        clientX: rect.x,
        clientY: rect.y,
        panel,
      });
    });
  }

  private renderCanvas(attrs: PanelContainerAttrs) {
    if (!this.ctx) return;
    if (!this.virtualCanvas) return;

    const ctx = this.ctx;
    const vc = this.virtualCanvas;
    const redrawStart = debugNow();

    ctx.resetTransform();
    ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);

    const dpr = window.devicePixelRatio;
    ctx.scale(dpr, dpr);
    ctx.translate(-vc.canvasRect.left, -vc.canvasRect.top);

    this.handleAreaSelection();

    const totalRenderedPanels = this.renderPanels(ctx, vc, attrs);

    this.drawTopLayerOnCanvas(ctx, vc);

    // Collect performance as the last thing we do.
    const redrawDur = debugNow() - redrawStart;
    this.updatePerfStats(
      redrawDur,
      this.panelInfos.length,
      totalRenderedPanels,
    );
  }

  private renderPanels(
    ctx: CanvasRenderingContext2D,
    vc: VirtualCanvas,
    attrs: PanelContainerAttrs,
  ): number {
    attrs.renderUnderlay?.(ctx, vc.size);

    let panelTop = 0;
    let totalOnCanvas = 0;

    const renderedPanels = Array<RenderedPanelInfo>();

    for (let i = 0; i < this.panelInfos.length; i++) {
      const {
        panel,
        width: panelWidth,
        height: panelHeight,
      } = this.panelInfos[i];

      const panelRect = {
        left: 0,
        top: panelTop,
        bottom: panelTop + panelHeight,
        right: panelWidth,
      };
      const panelSize = {width: panelWidth, height: panelHeight};

      if (vc.overlapsCanvas(panelRect)) {
        totalOnCanvas++;

        ctx.save();
        ctx.translate(0, panelTop);
        canvasClip(ctx, 0, 0, panelWidth, panelHeight);
        const beforeRender = debugNow();
        panel.renderCanvas(ctx, panelSize);
        this.updatePanelStats(
          i,
          panel,
          debugNow() - beforeRender,
          ctx,
          panelSize,
        );
        ctx.restore();
      }

      renderedPanels.push({
        panel,
        rect: {
          top: panelTop,
          bottom: panelTop + panelHeight,
          left: 0,
          right: panelWidth,
        },
      });

      panelTop += panelHeight;
    }

    attrs.renderOverlay?.(ctx, vc.size, renderedPanels);

    return totalOnCanvas;
  }

  // The panels each draw on the canvas but some details need to be drawn across
  // the whole canvas rather than per panel.
  private drawTopLayerOnCanvas(
    ctx: CanvasRenderingContext2D,
    vc: VirtualCanvas,
  ): void {
    const area = globals.timeline.selectedArea;
    if (
      area === undefined ||
      globals.timeline.areaY.start === undefined ||
      globals.timeline.areaY.end === undefined
    ) {
      return;
    }
    if (this.panelInfos.length === 0 || area.trackUris.length === 0) return;

    // Find the minY and maxY of the selected tracks in this panel container.
    let selectedTracksMinY = this.panelContainerHeight + this.panelContainerTop;
    let selectedTracksMaxY = this.panelContainerTop;
    let trackFromCurrentContainerSelected = false;
    for (let i = 0; i < this.panelInfos.length; i++) {
      const trackUri = this.panelInfos[i].trackNode?.uri;
      if (trackUri && area.trackUris.includes(trackUri)) {
        trackFromCurrentContainerSelected = true;
        selectedTracksMinY = Math.min(
          selectedTracksMinY,
          this.panelInfos[i].clientY,
        );
        selectedTracksMaxY = Math.max(
          selectedTracksMaxY,
          this.panelInfos[i].clientY + this.panelInfos[i].height,
        );
      }
    }

    // No box should be drawn if there are no selected tracks in the current
    // container.
    if (!trackFromCurrentContainerSelected) {
      return;
    }

    // TODO(stevegolton): We shouldn't know anything about visible time scale
    // right now, that's a job for our parent, but we can put one together so we
    // don't have to refactor this entire bit right now...

    const visibleTimeScale = new TimeScale(globals.timeline.visibleWindow, {
      left: 0,
      right: vc.size.width - TRACK_SHELL_WIDTH,
    });

    const startX = visibleTimeScale.timeToPx(area.start);
    const endX = visibleTimeScale.timeToPx(area.end);
    // To align with where to draw on the canvas subtract the first panel Y.
    selectedTracksMinY -= this.panelContainerTop;
    selectedTracksMaxY -= this.panelContainerTop;
    ctx.save();
    ctx.strokeStyle = SELECTION_STROKE_COLOR;
    ctx.lineWidth = 1;

    ctx.translate(TRACK_SHELL_WIDTH, 0);

    // Clip off any drawing happening outside the bounds of the timeline area
    canvasClip(ctx, 0, 0, vc.size.width - TRACK_SHELL_WIDTH, vc.size.height);

    ctx.strokeRect(
      startX,
      selectedTracksMaxY,
      endX - startX,
      selectedTracksMinY - selectedTracksMaxY,
    );
    ctx.restore();
  }

  private updatePanelStats(
    panelIndex: number,
    panel: Panel,
    renderTime: number,
    ctx: CanvasRenderingContext2D,
    size: Size2D,
  ) {
    if (!perfDebug()) return;
    let renderStats = this.panelPerfStats.get(panel);
    if (renderStats === undefined) {
      renderStats = new RunningStatistics();
      this.panelPerfStats.set(panel, renderStats);
    }
    renderStats.addValue(renderTime);

    // Draw a green box around the whole panel
    ctx.strokeStyle = 'rgba(69, 187, 73, 0.5)';
    const lineWidth = 1;
    ctx.lineWidth = lineWidth;
    ctx.strokeRect(
      lineWidth / 2,
      lineWidth / 2,
      size.width - lineWidth,
      size.height - lineWidth,
    );

    const statW = 300;
    ctx.fillStyle = 'hsl(97, 100%, 96%)';
    ctx.fillRect(size.width - statW, size.height - 20, statW, 20);
    ctx.fillStyle = 'hsla(122, 77%, 22%)';
    const statStr = `Panel ${panelIndex + 1} | ` + runningStatStr(renderStats);
    ctx.fillText(statStr, size.width - statW, size.height - 10);
  }

  private updatePerfStats(
    renderTime: number,
    totalPanels: number,
    panelsOnCanvas: number,
  ) {
    if (!perfDebug()) return;
    this.perfStats.renderStats.addValue(renderTime);
    this.perfStats.totalPanels = totalPanels;
    this.perfStats.panelsOnCanvas = panelsOnCanvas;
  }

  renderPerfStats() {
    return [
      m(
        'div',
        `${this.perfStats.totalPanels} panels, ` +
          `${this.perfStats.panelsOnCanvas} on canvas.`,
      ),
      m('div', runningStatStr(this.perfStats.renderStats)),
    ];
  }
}
