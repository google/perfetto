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
import {
  drawFloatingBubble,
  drawVerticalDashLine,
  measureTextWidth,
} from '../utils';
import {BaseNode} from './base';
import {Label} from '../label';
import {ENodeType, TMarkNodeData} from '../types';
import {IChartConfig} from '../../config';

export class MarkNode extends BaseNode<TMarkNodeData> {
  nodeType = ENodeType.Mark;

  private config: IChartConfig;

  private lineX: number;

  constructor(data: TMarkNodeData, level: number, config: IChartConfig) {
    super(data, level);
    this.config = config;
    const labelContent = data.name || '';
    this.label = new Label(labelContent, config);
    this.color = this.getNewColor(labelContent, false, false);
    this.lineX = 0;
  }

  shouldDraw(
    scale: ScaleLinear<number, number, never>,
    canvasWidth: number,
  ): boolean {
    this.lineX = scale(this.data.ts);
    if (this.lineX > canvasWidth || this.lineX + (this.label.width ?? 0) < 0) {
      this.pos = undefined;
      return false;
    }
    return true;
  }

  calPosition(
    ctx: CanvasRenderingContext2D,
    scale: ScaleLinear<number, number, never>,
    lastMarkerX: number,
    startLevel: number,
    offsetY: number,
  ) {
    const x = Math.floor(scale(this.data.ts));
    const newX = lastMarkerX <= x ? x : lastMarkerX;
    const width = Math.floor(measureTextWidth(ctx, this.label.content));
    const y =
      (this.level + startLevel) *
        ((this.config.node?.height ?? 0) + (this.config.node?.margin ?? 0)) -
      offsetY;

    this.pos = {
      x: newX,
      y,
      width,
      height: this.config.node?.height ?? 0,
    };
  }

  drawLine(
    ctx: CanvasRenderingContext2D,
    scale: ScaleLinear<number, number, never>,
    canvasWidth: number,
    canvasHeight: number,
  ) {
    if (!this.shouldDraw(scale, canvasWidth)) {
      return;
    }
    drawVerticalDashLine(ctx, {x: this.lineX}, canvasHeight, this.color);
  }

  drawCore(ctx: CanvasRenderingContext2D) {
    if (!this.pos) {
      return;
    }
    const markY = this.pos.y - this.pos.height / 2;
    const bubblePos = {
      x: this.lineX,
      y: markY,
      width: this.pos.width,
      height: this.pos.height,
    };

    drawFloatingBubble(ctx, bubblePos, this.color);

    this.label.draw(
      ctx,
      {
        x:
          this.lineX + bubblePos.height / 2 - (this.config.label?.padding ?? 0),
        y: markY,
      },
      this.pos.width + 2 * (this.config.label?.padding ?? 0),
    );
  }

  drawRect(
    ctx: CanvasRenderingContext2D,
    scale: ScaleLinear<number, number, never>,
    lastMarkerX: number,
    startLevel: number,
    canvasWidth: number,
    offsetY: number,
  ) {
    if (!this.shouldDraw(scale, canvasWidth)) {
      return;
    }
    this.calPosition(ctx, scale, lastMarkerX, startLevel, offsetY);
    this.drawCore(ctx);
  }

  /**
   * @deprecated
   */
  draw() {
    //
  }

  isHover(currentX: number, currentY: number): boolean {
    if (!this.pos) {
      return false;
    }
    const {x, y, width, height} = this.pos;
    if (
      currentX >= x &&
      currentX <= x + width + this.pos.height &&
      currentY >= y &&
      currentY <= y + height
    ) {
      return true;
    }
    return false;
  }

  updateColor(ctx: CanvasRenderingContext2D): void {
    if (this.pos && !(this.track || this.group).triangle.isCollapse) {
      this.drawCore(ctx);
    }
  }
}
