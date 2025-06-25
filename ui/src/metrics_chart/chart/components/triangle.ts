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

import {IChartConfig} from '../../config';
import {drawCanvasRect, drawCanvasTriangle} from '../utils';

export class Triangle {
  private config: IChartConfig;

  isCollapse = false;

  name = '';

  constructor(config: IChartConfig, name: string) {
    this.config = config;
    this.name = name;
  }

  private collapsePos: [number, number][] = [];

  private expandPos: [number, number][] = [];

  calTrianglePos(isCollapse: boolean, y: number, len = 10): [number, number][] {
    const height = len * Math.sin(Math.PI / 3);
    if (isCollapse) {
      const collapsedShift = ((this.config.node?.height ?? 0) - len) / 2;
      this.collapsePos = [
        [0, y + collapsedShift],
        [0, y + len + collapsedShift],
        [height, y + height / 2 + collapsedShift],
      ];
      return this.collapsePos;
    }

    const expandShift = ((this.config.node?.height ?? 0) - height) / 2;
    this.expandPos = [
      [len / 2, y + height + expandShift],
      [0, y + expandShift],
      [len, y + expandShift],
    ];
    return this.expandPos;
  }

  draw(ctx: CanvasRenderingContext2D, y: number, backgroundColor: string) {
    const width = this.config.node?.height ?? 0;
    const height = width;
    ctx.clearRect(0, y, width, height);
    drawCanvasRect(ctx, {x: 0, y, width, height}, backgroundColor);
    const pos = this.calTrianglePos(this.isCollapse, y);
    drawCanvasTriangle(ctx, pos);
  }

  isInTriangle([x, y]: [number, number]) {
    // Increase clickable area
    const mistake = 5;
    if (this.isCollapse) {
      const rightXBound = this.collapsePos[2][0];
      if (x > rightXBound + mistake) {
        return false;
      }
      const topYBound = this.collapsePos[0][1];
      const bottomYBound = this.collapsePos[1][1];
      if (y < topYBound - mistake) {
        return false;
      }
      if (y > bottomYBound + mistake) {
        return false;
      }
      return true;
    }
    const rightXBound = this.expandPos[2][0];
    if (x > rightXBound + mistake) {
      return false;
    }
    const topYBound = this.expandPos[2][1];
    const bottomYBound = this.expandPos[0][1];
    if (y < topYBound - mistake) {
      return false;
    }
    if (y > bottomYBound + mistake) {
      return false;
    }
    return true;
  }

  isClicked(pos: [number, number]) {
    if (this.isInTriangle(pos)) {
      this.isCollapse = !this.isCollapse;
      return true;
    }
    return false;
  }
}
