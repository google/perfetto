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
import {ENodeType, TBaseNodeData} from '../types';
import {GroupWithThread, Group, Track} from '../group';
import {Label} from '../label';
import {getColorForSlice} from '../../../components/colorizer';

export abstract class BaseNode<T extends TBaseNodeData = TBaseNodeData> {
  nodeType!: ENodeType;
  group!: Group | GroupWithThread;
  track: Track | undefined;

  readonly level: number;

  readonly data: T;

  protected readonly hoverTolerableError = 1;
  color!: string;
  pos:
    | {
        x: number;
        y: number;
        width: number;
        height: number;
      }
    | undefined;
  label!: Label;

  private isPrevDesaturate = false;

  constructor(data: T, level: number) {
    this.data = data;
    this.level = level;
  }

  abstract shouldDraw(
    scale: ScaleLinear<number, number, never>,
    canvasWidth: number,
  ): boolean;

  abstract draw(
    ctx: CanvasRenderingContext2D,
    scale: ScaleLinear<number, number, never>,
    startLevel: number,
    canvasWidth: number,
    offsetY: number,
  ): void;

  abstract updateColor(ctx: CanvasRenderingContext2D): void;

  abstract isHover(currentX: number, currentY: number): boolean;

  isClicked(currentX: number, currentY: number): boolean {
    return this.isHover(currentX, currentY);
  }

  lighten(ctx: CanvasRenderingContext2D): void {
    const newColor = this.getNewColor(this.label.content, true, false);
    if (!this.shouldUpdateColor(newColor)) {
      return;
    }
    this.color = newColor;
    this.updateColor(ctx);
  }

  desaturate(ctx: CanvasRenderingContext2D): void {
    this.isPrevDesaturate = true;
    const newColor = this.getNewColor(this.label.content, false, true);
    if (!this.shouldUpdateColor(newColor)) {
      return;
    }
    this.color = newColor;
    this.updateColor(ctx);
  }

  reset(ctx: CanvasRenderingContext2D): void {
    this.isPrevDesaturate = false;
    const newColor = this.getNewColor(this.label.content, false, false);
    if (!this.shouldUpdateColor(newColor)) {
      return;
    }
    this.color = newColor;
    this.updateColor(ctx);
  }

  restore(ctx: CanvasRenderingContext2D): void {
    if (this.isPrevDesaturate) {
      this.desaturate(ctx);
    } else {
      this.reset(ctx);
    }
  }

  getNewColor(content: string, isLight: boolean, _isGray: boolean) {
    const clorScheme = getColorForSlice(content);
    return isLight ? clorScheme.variant.cssString : clorScheme.base.cssString;
  }

  shouldUpdateColor(newColor: string) {
    return this.color !== newColor;
  }
}
