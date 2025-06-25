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
import {drawCanvasRect} from '../utils';
import {BaseNode} from './base';
import {ENodeType, TInstanceNodeData} from '../types';
import {IChartConfig} from '../../config';
import {Label} from '../label';

export class InstanceNode extends BaseNode<TInstanceNodeData> {
  nodeType = ENodeType.Instance;

  private config: IChartConfig;

  constructor(data: TInstanceNodeData, level: number, config: IChartConfig) {
    super(data, level);
    this.config = config;
    const labelContent = data.name || '';
    this.label = new Label(labelContent, config);
    this.color = this.getNewColor(labelContent, false, false);
  }

  shouldDraw(
    scale: ScaleLinear<number, number, never>,
    canvasWidth: number,
  ): boolean {
    const start = scale(this.data.ts);
    if (start > canvasWidth || start + 1 < 0) {
      this.pos = undefined;
      return false;
    }
    return true;
  }

  calPosition(
    scale: ScaleLinear<number, number, never>,
    startLevel: number,
    offsetY: number,
  ) {
    const x = Math.floor(scale(this.data.ts));
    const y =
      (this.level + startLevel) *
        ((this.config.node?.height ?? 0) + (this.config.node?.margin ?? 0)) -
      offsetY;
    const pos = {
      x,
      y,
      width: 1,
      height: this.config.node?.height ?? 0,
    };
    this.pos = pos;

    return pos;
  }

  draw(
    ctx: CanvasRenderingContext2D,
    scale: ScaleLinear<number, number, never>,
    startLevel: number,
    canvasWidth: number,
    offsetY: number,
  ): void {
    if (!this.shouldDraw(scale, canvasWidth)) {
      return;
    }
    this.calPosition(scale, startLevel, offsetY);
    drawCanvasRect(ctx, this.pos, this.color);
  }

  isHover(currentX: number, currentY: number): boolean {
    if (!this.pos) {
      return false;
    }
    if (
      currentX >= this.pos.x - this.hoverTolerableError &&
      currentX <= this.pos.x + this.hoverTolerableError &&
      currentY <= this.pos.y + (this.config.node?.height ?? 0) &&
      currentY >= this.pos.y
    ) {
      return true;
    }
    return false;
  }

  updateColor(ctx: CanvasRenderingContext2D): void {
    if (this.pos && !(this.track || this.group).triangle.isCollapse) {
      ctx.clearRect(this.pos.x, this.pos.y, this.pos.width, this.pos.height);
      drawCanvasRect(ctx, this.pos, this.color);
    }
  }
}
