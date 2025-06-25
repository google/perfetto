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

import {getFontContent, IChartConfig} from '../../config';
import {drawCanvasLabel, measureTextWidth} from '../utils';
import {Triangle} from './triangle';

export class Header {
  position: {x: number; y: number; width: number; height: number};

  private flag = false;

  private name: string;

  private config: IChartConfig;

  private triangleDelegate: Triangle;

  constructor(name: string, config: IChartConfig, triangleDelegate: Triangle) {
    this.name = name;
    this.config = config;
    this.triangleDelegate = triangleDelegate;
    this.position = {
      x: (this.config.label?.padding ?? 0) + (this.config.node?.height ?? 0),
      y: 0,
      width: 0,
      height: (this.config.node?.height ?? 0) + (this.config.node?.margin ?? 0),
    };
  }

  draw(ctx: CanvasRenderingContext2D, y: number, backgroundColor: string) {
    if (this.flag === false) {
      this.position.width = measureTextWidth(ctx, this.name);
      this.flag = true;
    }
    this.drawHeaderText(ctx, y);
    this.triangleDelegate.draw(ctx, y, backgroundColor);
    this.position.y = y;
  }

  drawHeaderText(ctx: CanvasRenderingContext2D, y: number) {
    drawCanvasLabel(
      ctx,
      {
        x: (this.config.label?.padding ?? 0) + (this.config.node?.height ?? 0),
        y:
          y +
          Math.floor(
            ((this.config.node?.height ?? 0) +
              (this.config.node?.margin ?? 0)) /
              2,
          ),
      },
      this.name,
      getFontContent(this.config),
      'black',
    );
  }

  isHover(currentX: number, currentY: number) {
    const {x, y, width, height} = this.position;
    if (
      currentX >= x &&
      currentX <= x + width &&
      currentY >= y &&
      currentY <= y + height
    ) {
      return true;
    }
    return false;
  }

  isClick(currentX: number, currentY: number) {
    return this.isHover(currentX, currentY);
  }
}
