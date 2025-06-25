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
import {drawCanvasArrow} from '../utils';
import {ENodeType, TZeroDurationNodeData} from '../types';
import {IChartConfig} from '../../config';
import {Label} from '../label';

export class ZeroDurationNode extends BaseNode<TZeroDurationNodeData> {
  nodeType = ENodeType.ZeroDuration;

  private config: IChartConfig;

  constructor(
    data: TZeroDurationNodeData,
    level: number,
    config: IChartConfig,
  ) {
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
    const center = scale(this.data.ts);
    if (center - 5 > canvasWidth || center + 5 < 0) {
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
    this.pos = {
      x,
      y,
      width: (this.config.node?.height ?? 0) * 0.7,
      height: this.config.node?.height ?? 0,
    };
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
    drawCanvasArrow(ctx, this.pos, this.color);
  }

  isHover(currentX: number, currentY: number): boolean {
    if (!this.pos) {
      return false;
    }
    const {x, y, width, height} = this.pos;
    if (currentX < x - width / 2) {
      return false;
    }
    if (currentX > x + width / 2) {
      return false;
    }
    if (currentX === x) {
      if (currentY < y) {
        return false;
      }
      if (currentY > y + width) {
        return false;
      }
      return true;
    }
    const ratioTop = width / 2 / height;
    if (currentY < y + Math.abs(currentX - this.pos.x) / ratioTop) {
      return false;
    }
    const ratioBottom = width / 2 / (height - width);
    if (currentY > y + width + (height - width) / ratioBottom) {
      return false;
    }
    return true;
  }

  updateColor(ctx: CanvasRenderingContext2D): void {
    if (this.pos && !(this.track || this.group).triangle.isCollapse) {
      drawCanvasArrow(ctx, this.pos, this.color);
    }
  }
}
