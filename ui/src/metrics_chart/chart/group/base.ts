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

import {ScaleLinear} from 'd3';
import {DurationNode, MarkNode} from '../node';
import {TNode, TNodeData} from '../types';
import {IChartConfig} from '../../config';
import {Triangle} from '../components/triangle';
import {Header} from '../components/header';

export class CommonBase {
  name: string;

  nodes: TNode[];

  config: IChartConfig;

  startLevel: number;

  maxLevel: number;

  // Distance from the top of the canvas
  y: number;

  // The height of the entire area
  height: number;

  triangle: Triangle;

  header: Header;

  backgroundColor = '';

  data: TNodeData[] = [];

  isAsyncEvent = false;

  constructor(
    name: string,
    nodes: TNode[],
    config: IChartConfig,
    startLevel: number,
    maxLevel: number,
  ) {
    this.name = name;
    this.nodes = nodes;
    this.data = this.nodes.map((node) => node.data);
    this.config = config;
    this.startLevel = startLevel;
    this.y =
      startLevel *
      ((this.config.node?.height ?? 0) + (this.config.node?.margin ?? 0));
    this.height =
      maxLevel *
      ((this.config.node?.height ?? 0) + (this.config.node?.margin ?? 0));
    this.maxLevel = maxLevel;
    this.triangle = new Triangle(config, name);
    this.header = new Header(name, config, this.triangle);
  }

  drawBackground(
    ctx: CanvasRenderingContext2D | undefined | null,
    idx: number,
    width: number,
    offsetY: number,
  ) {
    if (!ctx) {
      return;
    }
    this.backgroundColor =
      idx % 2 === 0 ? 'rgb(237, 239, 241)' : 'rgb(255,255,255)';
    ctx.beginPath();
    ctx.rect(0, this.y - offsetY, width, this.height);
    ctx.fillStyle = `${this.backgroundColor}`;
    ctx.fill();
    ctx.closePath();
  }

  drawHeader(
    ctx: CanvasRenderingContext2D | undefined | null,
    offsetY: number,
  ) {
    if (ctx) {
      this.header.draw(ctx, this.y - offsetY, this.backgroundColor);
    }
  }

  draw(
    ctx: CanvasRenderingContext2D | undefined | null,
    scale: ScaleLinear<number, number, never> | undefined,
    canvasWidth: number,
    offsetY: number,
  ) {
    if (this.triangle.isCollapse || !ctx || !scale) {
      return;
    }

    this.drawNodes(ctx, scale, canvasWidth, offsetY);
    this.drawRectInMarkNodes(ctx, scale, canvasWidth, offsetY);
  }

  drawNodes(
    ctx: CanvasRenderingContext2D,
    scale: ScaleLinear<number, number, never>,
    canvasWidth: number,
    offsetY: number,
  ) {
    const withMarkNodes = this.nodes.find((node) => node instanceof MarkNode);

    this.nodes.forEach((node, idx) => {
      if (idx === 0 && node instanceof DurationNode && this.isAsyncEvent) {
        node.setShouldDrawTopBorder(true);
      }
      let level = this.startLevel;
      if (!(node instanceof MarkNode) && withMarkNodes) {
        level += 1;
      }

      node.draw(ctx, scale, level, canvasWidth, offsetY);
    });
  }

  drawRectInMarkNodes(
    ctx: CanvasRenderingContext2D,
    scale: ScaleLinear<number, number, never>,
    canvasWidth: number,
    offsetY: number,
  ) {
    let lastMarkerX = -Infinity;
    this.nodes.forEach((node) => {
      if (node instanceof MarkNode) {
        node.drawRect(
          ctx,
          scale,
          lastMarkerX,
          this.startLevel,
          canvasWidth,
          offsetY,
        );
        const currentMarkerX =
          node.pos != null ? node.pos.x + node.pos.width : -Infinity;
        lastMarkerX = currentMarkerX;
      }
    });
  }

  containsMarkNodes() {
    return this.nodes.find((node) => node instanceof MarkNode) !== undefined;
  }

  drawLineInMarkNodes(
    ctx: CanvasRenderingContext2D | undefined | null,
    scale: ScaleLinear<number, number, never> | undefined,
    canvasWidth: number,
    canvasHeight: number,
  ) {
    if (this.triangle.isCollapse || !ctx || !scale) {
      return;
    }

    this.nodes.forEach((node) => {
      if (node instanceof MarkNode) {
        node.drawLine(ctx, scale, canvasWidth, canvasHeight);
      }
    });
  }

  isHover(currentY: number) {
    if (currentY < this.y) {
      return false;
    }
    if (currentY > this.y + this.height) {
      return false;
    }
    return true;
  }

  isClick(currentY: number) {
    return this.isHover(currentY);
  }
}
