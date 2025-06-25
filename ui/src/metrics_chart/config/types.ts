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

import {Selection} from 'd3';
import {TTooltipNodeData} from '../chart/components';
import {TNode} from '../chart/types';

export interface IChartConfig {
  /**
   * basis of all chart data, the final chart data is (chart data - basis)
   * @default 0
   */
  basis?: number;
  label?: {
    /**
     * padding left and padding right
     */
    padding?: number;
    fontSize?: number;
    fontFamily?: string;
  };
  /**
   * chart zoom scale
   * @default [1,10000]
   */
  scale?: false | [number, number];
  /**
   * the style of chart node
   */
  node?: {
    height?: number;
    /**
     * marargin top of adjacent nodes
     */
    margin?: number;
  };
  /**
   * config x axis
   */
  xAxis?: {
    /**
     * config label content in x axis
     * @param value original label content in x axis
     * @param index index
     * @returns new label content
     */
    valueFormatter: (
      value: number | {valueOf: () => number},
      index: number,
    ) => string;
    /**
     * config x axis style
     * @param text label content in x axis
     * @returns
     */
    style: (
      text: Selection<SVGTextElement, number, SVGGElement, unknown>,
    ) => void;
  };
  /**
   * tooltip config
   */
  tooltip?: {
    /**
     * modify node tooltip html content
     * @param val the value of current node
     * @param config current chart config
     * @returns tooltip html content for node
     */
    nodeFormatter?: (val: TTooltipNodeData, config: IChartConfig) => string;
  };
  maxCanvasHeight?: number;
}

export interface IChartEvents {
  onClickNode?: (node: TNode) => void;
}
