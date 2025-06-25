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
import {TNode} from '../types';
import {IChartConfig} from '../../config';
import {AsyncTrack, Track} from './track';
import {CommonBase} from './base';

export class Group extends CommonBase {}

export class GroupWithThread extends Group {
  readonly thread: (Track | AsyncTrack)[];

  constructor(
    name: string,
    nodes: TNode[],
    config: IChartConfig,
    startLevel: number,
    maxLevel: number,
    thread: Track[],
  ) {
    super(name, nodes, config, startLevel, maxLevel);
    this.thread = thread;
  }

  draw(
    ctx: CanvasRenderingContext2D | undefined | null,
    scale: ScaleLinear<number, number, never> | undefined,
    canvasWidth: number,
    offsetY: number,
  ) {
    if (this.triangle.isCollapse) {
      return;
    }
    this.thread.forEach((item, idx) => {
      item.drawBackground(ctx, idx, canvasWidth, offsetY);
      item.drawHeader(ctx, offsetY);
      item.draw(ctx, scale, canvasWidth, offsetY);
    });
  }
}
