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
import {BaseNode} from './base';
import {drawCanvasRect} from '../utils';
import {Label} from '../label';
import {ENodeType, TDurationNodeData} from '../types';
import {IChartConfig} from '../../config';

export class DurationNode extends BaseNode<TDurationNodeData> {
  nodeType = ENodeType.Duration;

  private config: IChartConfig;

  private shouldDrawTopBorder = false;

  private labelColor = '#fdfdfd';

  constructor(data: TDurationNodeData, level: number, config: IChartConfig) {
    super(data, level);

    this.config = config;

    const labelContent = data.name || '';
    this.color = this.getNewColor(labelContent, false, false);

    this.label = new Label(labelContent, config);
  }

  shouldDraw(scale: ScaleLinear<number, number, never>, canvasWidth: number) {
    if (
      scale(this.data.ts) > canvasWidth ||
      scale(this.data.ts + this.data.dur) < 0
    ) {
      this.pos = undefined;
      return false;
    }
    return true;
  }

  calPosition(
    scale: ScaleLinear<number, number, never>,
    startLevel: number,
    canvasWidth: number,
    offsetY: number,
  ) {
    const {ts, dur} = this.data;
    const scaleS = Math.floor(scale(ts));
    const scaleE = Math.floor(scale(ts + dur));
    const x = scaleS < 0 ? 0 : scaleS;
    const width = (scaleE > canvasWidth ? canvasWidth : scaleE) - x;
    const y =
      (this.level + startLevel) *
        ((this.config.node?.height ?? 0) + (this.config.node?.margin ?? 0)) -
      offsetY;
    const pos = {
      x,
      y,
      width: width >= 1 ? width : 1,
      height: this.config.node?.height ?? 0,
    };
    this.pos = pos;
    return pos;
  }

  private drawCore(ctx: CanvasRenderingContext2D) {
    drawCanvasRect(ctx, this.pos, this.color, this.shouldDrawTopBorder);
    if (this.pos) {
      this.label.draw(ctx, this.pos, this.pos.width, this.labelColor);
    }
  }

  draw(
    ctx: CanvasRenderingContext2D,
    scale: ScaleLinear<number, number, never>,
    startLevel: number,
    canvasWidth: number,
    offsetY: number,
  ) {
    if (!this.shouldDraw(scale, canvasWidth)) {
      return;
    }
    this.calPosition(scale, startLevel, canvasWidth, offsetY);
    this.drawCore(ctx);
  }

  setShouldDrawTopBorder(val: boolean) {
    this.shouldDrawTopBorder = val;
  }

  isHover(currentX: number, currentY: number): boolean {
    if (!this.pos) {
      return false;
    }
    const {x, y, width} = this.pos;
    if (
      currentX >= x &&
      currentX <= x + width &&
      currentY <= y + (this.config.node?.height ?? 0) &&
      currentY >= y
    ) {
      return true;
    }
    return false;
  }

  updateColor(ctx: CanvasRenderingContext2D): void {
    if (this.pos && !(this.track || this.group).triangle.isCollapse) {
      ctx.clearRect(this.pos.x, this.pos.y, this.pos.width, this.pos.height);
      this.drawCore(ctx);
    }
  }
}
