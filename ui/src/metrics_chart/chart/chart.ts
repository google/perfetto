// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {select, pointer, ScaleLinear} from 'd3';
import merge from 'deepmerge';
import {ChartDataProvider} from '../data';
import {Tooltip} from './components';
import {TNode} from './types';
import {AsyncTrack, Group, GroupWithThread, Track} from './group';
import {ViewPort} from './viewport';
import {defaultConfig, IChartConfig, IChartEvents} from '../config';
import {Flow} from './flow';
import {TTraceEvent} from '../types';
import {drawHighlightArea} from './utils';

export class Chart {
  readonly config: IChartConfig;

  readonly events: IChartEvents;

  canvasHeight: number;

  private viewPort: ViewPort;

  chartDataProvider: ChartDataProvider;

  private groups: (Group | GroupWithThread)[];

  private flatGroups: (Group | GroupWithThread | Track | AsyncTrack)[];

  private tooltip: Tooltip;

  private collapsedHeight: number;

  private flow: Flow;

  private isDragging = false;

  private pendingMouseDownEvent: MouseEvent | undefined;

  private currentMouseMoveEvent: MouseEvent | undefined;

  constructor({
    container,
    data,
    config,
    events,
  }: {
    container: HTMLDivElement;
    data: TTraceEvent[];
    config: IChartConfig;
    events: IChartEvents;
  }) {
    this.canvasHeight = 0;
    this.flatGroups = [];
    this.config = merge(defaultConfig, config);
    this.events = events;
    this.chartDataProvider = new ChartDataProvider(data);
    this.collapsedHeight =
      (this.config?.node?.height ?? 0) + (this.config?.node?.margin ?? 0);
    this.viewPort = new ViewPort(
      {
        node: container,
        selection: select(container),
      },
      this,
    );
    this.groups = this.chartDataProvider.groups(this.config);
    this.flow = new Flow(data);
    this.tooltip = new Tooltip(this.config);
    this.draw();
  }

  get scale(): ScaleLinear<number, number, never> | undefined {
    if (this.viewPort.transform != null) {
      return this.viewPort.transform.rescaleX(this.viewPort.originalScale);
    }
    return this.viewPort.originalScale;
  }

  close() {
    this.viewPort.close();
    this.tooltip.selection.remove();
  }

  update() {
    const ratio = window.devicePixelRatio || 1;
    const w = (this.viewPort.canvasContainer?.node.clientWidth ?? 0) * ratio;
    const h = (this.viewPort.canvasContainer?.node.clientHeight ?? 0) * ratio;
    this.viewPort.xAxis?.zoomXAxis(this.scale);
    this.viewPort.canvas?.ctx?.clearRect(0, 0, w, h);
    this.draw();
  }

  private draw() {
    this.updateChartRelatedInfo();
    if (this.viewPort.hasInit) {
      this.viewPort.update(this.canvasHeight);
    } else {
      this.viewPort.init(this.canvasHeight);
    }
    this.drawGroup();
    this.flow.draw(
      this.viewPort?.canvas?.ctx,
      this.chartDataProvider.eventToNode,
      this.config,
      this.scale,
      this.viewPort.canvasContainer?.node.clientWidth ?? 0,
      this.viewPort.canvasContainer?.node.clientHeight ?? 0,
      this.viewPort.offsetY,
    );
    this.drawHighlightArea();
  }

  private updateChartRelatedInfo() {
    let prevTop = 0;
    let startLevel = 0;
    let canvasHeight = 0;
    this.groups.forEach((g, idx) => {
      const {isCollapse} = g.triangle;
      g.y = prevTop;
      g.startLevel = startLevel;
      if (isCollapse) {
        g.height = this.collapsedHeight;
        prevTop += this.collapsedHeight;
        startLevel += 1;
      } else {
        if (g instanceof GroupWithThread) {
          const maxLevel =
            g.thread.reduce((accu, thread, index) => {
              if (index === 0) {
                prevTop +=
                  (this.config.node?.height ?? 0) +
                  (this.config.node?.margin ?? 0);
                startLevel += 1;
              }
              thread.y = prevTop;
              thread.startLevel = startLevel;
              if (thread.triangle.isCollapse) {
                thread.height = this.collapsedHeight;
                prevTop += this.collapsedHeight;
                startLevel += 1;
              } else {
                thread.height =
                  thread.maxLevel *
                  ((this.config.node?.height ?? 0) +
                    (this.config.node?.margin ?? 0));
                prevTop += thread.height || 0;
                startLevel += thread.maxLevel || 0;
              }
              accu += thread.triangle.isCollapse ? 1 : thread.maxLevel;
              return accu;
            }, 0) + 1;

          // for group header
          g.height =
            maxLevel *
            ((this.config.node?.height ?? 0) + (this.config.node?.margin ?? 0));
        } else {
          g.height =
            g.maxLevel *
            ((this.config.node?.height ?? 0) + (this.config.node?.margin ?? 0));
          prevTop += g.height || 0;
          startLevel += this.groups[idx].maxLevel || 0;
        }
      }
      canvasHeight += g.height;
    });
    this.canvasHeight = canvasHeight;
  }

  private drawGroup() {
    this.groups.forEach((g, idx) => {
      g.drawBackground(
        this.viewPort.canvas?.ctx,
        idx,
        this.viewPort.canvasContainer?.node.clientWidth ?? 0,
        this.viewPort.offsetY,
      );
      g.drawHeader(this.viewPort.canvas?.ctx, this.viewPort.offsetY);
    });
    this.groups.forEach((g) => {
      g.draw(
        this.viewPort.canvas?.ctx,
        this.scale,
        this.viewPort.canvasContainer?.node.clientWidth ?? 0,
        this.viewPort.offsetY,
      );
    });

    this.drawMarkNodes();
  }

  private drawMarkNodes() {
    this.groups.forEach((g) => {
      g.drawLineInMarkNodes(
        this.viewPort.canvas?.ctx,
        this.scale,
        this.viewPort.canvasContainer?.node.clientWidth ?? 0,
        this.viewPort.canvasContainer?.node.clientHeight ?? 0,
      );
    });
  }

  private drawHighlightArea() {
    if (this.pendingMouseDownEvent && this.currentMouseMoveEvent) {
      drawHighlightArea(this.viewPort.canvas?.ctx, this.scale, {
        startX: this.pendingMouseDownEvent.offsetX,
        startY: this.pendingMouseDownEvent.offsetY,
        endX: this.currentMouseMoveEvent.offsetX,
        endY: this.currentMouseMoveEvent.offsetY,
      });
    }
  }

  private getHoverNodeForGroup(
    group: Group,
    currentX: number,
    currentY: number,
  ) {
    if (group.triangle.isCollapse) {
      return;
    }

    let hoverNode: TNode | undefined = undefined;

    group.nodes.forEach((node) => {
      if (this.viewPort.canvas?.ctx) {
        if (node.isHover(currentX, currentY)) {
          hoverNode = node;
          node.lighten(this.viewPort.canvas.ctx);
        } else {
          node.restore(this.viewPort.canvas.ctx);
        }
      }
    });

    return hoverNode;
  }

  private getHoverNodeForGroupWithThread(
    group: GroupWithThread,
    currentX: number,
    currentY: number,
  ) {
    if (group.triangle.isCollapse) {
      return;
    }

    let hoverNode: TNode | undefined = undefined;

    group.thread.forEach((thread) => {
      if (!thread.triangle.isCollapse) {
        thread.nodes.forEach((node) => {
          if (this.viewPort.canvas?.ctx) {
            if (node.isHover(currentX, currentY)) {
              hoverNode = node;
              node.lighten(this.viewPort.canvas.ctx);
            } else {
              node.restore(this.viewPort.canvas.ctx);
            }
          }
        });
      }
    });

    return hoverNode;
  }

  private getHoverNode(
    group: Group | GroupWithThread,
    currentX: number,
    currentY: number,
  ): TNode | undefined {
    if (group instanceof GroupWithThread) {
      return this.getHoverNodeForGroupWithThread(group, currentX, currentY);
    } else {
      return this.getHoverNodeForGroup(group, currentX, currentY);
    }
  }

  private getClickedNode(
    group: Group | GroupWithThread,
    currentX: number,
    currentY: number,
  ) {
    return this.getHoverNode(group, currentX, currentY);
  }

  mouseDown(e: MouseEvent) {
    this.pendingMouseDownEvent = e;
  }

  mouseMove(e: MouseEvent) {
    if (
      !this.isDragging &&
      this.pendingMouseDownEvent &&
      (Math.abs(e.offsetX - this.pendingMouseDownEvent.offsetX) > 1 ||
        Math.abs(e.offsetY - this.pendingMouseDownEvent.offsetY) > 1)
    ) {
      this.isDragging = true;
      this.tooltip.hide();
    }
    // select area
    if (this.isDragging) {
      this.currentMouseMoveEvent = e;
      this.update();
    }
    // tooltip
    else {
      const position = {
        pageX: e.pageX,
        pageY: e.pageY,
        offsetX: e.offsetX,
        offsetY: e.offsetY,
      };

      let shouldChangeCursorToPointer = false;

      if (e.offsetY > (this.viewPort.canvasContainer?.node.clientHeight ?? 0)) {
        this.tooltip.hide();
        return;
      }

      this.groups.forEach((group) => {
        if (group.header.isHover(e.offsetX, e.offsetY)) {
          this.tooltip.show(
            {
              type: 'header',
              data: {
                headerName: group.name,
                nodes: group.nodes.map((node) => ({
                  nodeData: node.data,
                  nodeType: node.nodeType,
                })),
              },
            },
            position,
            this.viewPort.canvasContainer?.node.clientWidth ?? 0,
            this.viewPort.canvasContainer?.node.clientHeight ?? 0,
          );
        } else if (group.isHover(e.offsetY)) {
          const hoverNode = this.getHoverNode(group, e.offsetX, e.offsetY);
          if (hoverNode != null) {
            this.tooltip.show(
              {
                type: 'node',
                data: {
                  nodeData: hoverNode.data,
                  nodeType: hoverNode.nodeType,
                },
              },
              position,
              this.viewPort.canvasContainer?.node.clientWidth ?? 0,
              this.viewPort.canvasContainer?.node.clientHeight ?? 0,
            );

            shouldChangeCursorToPointer = true;
          } else if (group instanceof GroupWithThread) {
            const thread = group.thread.find((item) => item.isHover(e.offsetY));
            if (thread) {
              this.tooltip.show(
                {
                  type: 'thread',
                  data: {
                    threadName: thread.name,
                    nodes: thread.nodes.map((node) => ({
                      nodeData: node.data,
                      nodeType: node.nodeType,
                    })),
                  },
                },
                position,
                this.viewPort.canvasContainer?.node.clientWidth ?? 0,
                this.viewPort.canvasContainer?.node.clientHeight ?? 0,
              );
            } else {
              this.tooltip.show(
                {
                  type: 'group',
                  data: {
                    groupName: group.name,
                    nodes: group.nodes.map((node) => ({
                      nodeData: node.data,
                      nodeType: node.nodeType,
                    })),
                  },
                },
                position,
                this.viewPort.canvasContainer?.node.clientWidth ?? 0,
                this.viewPort.canvasContainer?.node.clientHeight ?? 0,
              );
            }
          } else {
            this.tooltip.show(
              {
                type: 'group',
                data: {
                  groupName: group.name,
                  nodes: group.nodes.map((node) => ({
                    nodeData: node.data,
                    nodeType: node.nodeType,
                  })),
                },
              },
              position,
              this.viewPort.canvasContainer?.node.clientWidth ?? 0,
              this.viewPort.canvasContainer?.node.clientHeight ?? 0,
            );
          }
        }
      });

      this.viewPort.canvas?.selection.style(
        'cursor',
        shouldChangeCursorToPointer ? 'pointer' : 'unset',
      );
    }
  }

  mouseUp(e: MouseEvent) {
    this.pendingMouseDownEvent = undefined;
    this.currentMouseMoveEvent = undefined;
    if (this.isDragging) {
      this.isDragging = false;
      this.update();
    } else {
      if (this.flatGroups.length <= 0) {
        this.flatGroups = this.groups.reduce(
          (prev: (Group | GroupWithThread | Track | AsyncTrack)[], g) => {
            if (g instanceof GroupWithThread) {
              prev.push(g);
              prev.push(...g.thread);
            } else {
              prev.push(g);
            }
            return prev;
          },
          [],
        );
      }
      for (const flatGroup of this.flatGroups) {
        const isParentGroupCollacpse =
          (flatGroup as Track | AsyncTrack).group != null &&
          (flatGroup as Track | AsyncTrack).group?.triangle.isCollapse;
        if (
          !isParentGroupCollacpse &&
          flatGroup.triangle.isClicked(pointer(e))
        ) {
          this.update();
          return;
        }
      }
      for (const group of this.groups) {
        if (group.header.isClick(e.offsetX, e.offsetY)) {
          return;
        }
        if (group.isClick(e.offsetY)) {
          const clickedNode = this.getClickedNode(group, e.offsetX, e.offsetY);
          if (clickedNode) {
            this.events.onClickNode && this.events.onClickNode(clickedNode);
            return;
          }
        }
      }
    }
  }

  mouseOut(_e: MouseEvent) {
    this.tooltip.hide();
  }
}
