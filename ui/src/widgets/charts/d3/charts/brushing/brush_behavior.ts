// Copyright (C) 2026 The Android Open Source Project
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

import * as d3 from 'd3';
import {Row, ChartSpec, Filter} from '../../data/types';

export interface BrushSelection {
  type: '1d' | '2d';
  extent: [number, number] | [[number, number], [number, number]];
}

export interface BrushResult {
  filters: Filter[];
  selectedData: Row[];
}

export interface BrushScales {
  x?: d3.ScaleBand<string> | d3.ScaleLinear<number, number>;
  y?: d3.ScaleLinear<number, number>;
}

export abstract class BrushBehavior {
  abstract createBrush(
    width: number,
    height: number,
  ): d3.BrushBehavior<unknown>;

  abstract onBrushEnd(
    selection: BrushSelection | null,
    data: Row[],
    spec: ChartSpec,
    scales: BrushScales,
  ): BrushResult;
}
