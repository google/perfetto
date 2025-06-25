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

import {axisTop, Selection, ScaleLinear, Axis, NumberValue, select} from 'd3';
import {IChartConfig} from '../../config';
import {IChartContainer} from '../types';
import {drawSvg, drawSvgZeroHeight} from '../utils';

export class XAxis {
  private scale: ScaleLinear<number, number, never>;

  private config: IChartConfig;

  private gxAxis: Selection<SVGGElement, unknown, null, undefined> | undefined;

  private xAxis: Axis<NumberValue> | undefined;

  readonly height: number;

  constructor(scale: ScaleLinear<number, number, never>, config: IChartConfig) {
    this.scale = scale;
    this.config = config;
    this.height = this.getHeight();
  }

  draw(xAxisContainer: IChartContainer) {
    const y = this.height;
    if (this.config.xAxis) {
      this.xAxis = axisTop(this.scale).tickFormat(
        this.config.xAxis.valueFormatter,
      );
    }

    const svg = drawSvg(xAxisContainer, this.height);
    if (this.xAxis) {
      this.gxAxis = svg
      .append('g')
      .attr('transform', `translate(0, ${y})`)
      .call(this.xAxis);
    }

    if (this.config.xAxis && this.gxAxis) {
      this.gxAxis
      .selectAll<SVGTextElement, number>('text')
      .call(this.config.xAxis.style);
    }
  }

  zoomXAxis(scale: ScaleLinear<number, number, never> | undefined) {
    if (this.gxAxis && this.xAxis && scale && this.config.xAxis) {
      this.gxAxis.call(this.xAxis.scale(scale));
      this.gxAxis
        .selectAll<SVGTextElement, number>('text')
        .call(this.config.xAxis?.style);
    }
  }

  private getHeight() {
    const svg = drawSvgZeroHeight(select(window.document.body));
    const gxAxis = svg.append('g').call(axisTop(this.scale));
    if (this.config.xAxis) {
      gxAxis
      .selectAll<SVGTextElement, number>('text')
      .call(this.config.xAxis?.style);
    }
    const height = Math.ceil(
      gxAxis.node()?.getBoundingClientRect().height ?? 0,
    );
    svg.remove();
    return height;
  }
}
