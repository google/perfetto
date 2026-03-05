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
import {BaseRenderer} from './base_renderer';
import {Row, ChartSpec, ChartType, FilterOp} from '../data/types';
import {RangeBrush} from './brushing';
import {TooltipFormatter} from '../tooltip';

/** Default number of bins for histograms when not specified */
export const DEFAULT_HISTOGRAM_BINS = 20;

type HistogramSpec = Extract<ChartSpec, {type: ChartType.Histogram}>;

/** Pre-aggregated histogram bin from DataSource */
interface HistogramBin {
  x0: number;
  x1: number;
  count: number;
}

interface HistogramScales {
  x: d3.ScaleLinear<number, number>;
  y: d3.ScaleLinear<number, number>;
}

export class HistogramRenderer extends BaseRenderer {
  constructor() {
    super();
    this.brushBehavior = new RangeBrush();
  }

  render(svg: SVGElement, data: Row[], spec: ChartSpec) {
    if (spec.type !== ChartType.Histogram) return;

    this.clear(svg);
    const {width, height} = this.getDimensions(svg);
    const g = this.createGroup(svg);

    if (data.length === 0) {
      this.renderEmpty(g, data, spec, width, height);
      return;
    }

    // Data is pre-aggregated with {x0, x1, count} structure
    const bins = data as unknown as HistogramBin[];

    // Validate bins have required fields
    const validBins = bins.filter(
      (b) =>
        b.x0 !== undefined &&
        b.x1 !== undefined &&
        b.count !== undefined &&
        !isNaN(b.x0) &&
        !isNaN(b.x1),
    );

    if (validBins.length === 0) {
      this.renderEmpty(g, data, spec, width, height);
      return;
    }

    const scales = this.createScales(validBins, width, height);

    this.addGridLines(g, scales.x, scales.y, width, height, false);
    this.setupBrush(g, data, spec, {x: scales.x}, width, height);
    this.drawBars(g, validBins, spec, scales, height);
    this.drawAxes(
      g,
      scales.x,
      scales.y,
      width,
      height,
      spec.x,
      'Frequency',
      (d) => d3.format('.2s')(Number(d)),
    );
  }

  private renderEmpty(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    data: Row[],
    spec: HistogramSpec,
    width: number,
    height: number,
  ) {
    this.renderEmptyState(g, width, height);

    const dummyX = d3.scaleLinear().domain([0, 100]).range([0, width]);
    this.setupBrush(g, data, spec, {x: dummyX}, width, height);
  }

  private createScales(
    bins: HistogramBin[],
    width: number,
    height: number,
  ): HistogramScales {
    return {
      x: d3
        .scaleLinear()
        .domain([bins[0].x0, bins[bins.length - 1].x1])
        .range([0, width]),
      y: d3
        .scaleLinear()
        .domain([0, d3.max(bins, (d) => d.count) ?? 0])
        .nice()
        .range([height, 0]),
    };
  }

  private drawBars(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    bins: HistogramBin[],
    spec: HistogramSpec,
    scales: HistogramScales,
    height: number,
  ) {
    const bars = g
      .selectAll<SVGRectElement, HistogramBin>('.bar')
      .data(bins)
      .join('rect')
      .attr('class', 'bar selectable')
      .attr('x', (d) => scales.x(d.x0) + 1)
      .attr('y', (d) => scales.y(d.count))
      .attr('width', (d) => Math.max(0, scales.x(d.x1) - scales.x(d.x0) - 2))
      .attr('height', (d) => height - scales.y(d.count))
      .attr('fill', 'steelblue')
      .style('cursor', 'pointer')
      .style('pointer-events', 'all')
      .on('click', (_event, d) => {
        this.handleBarClick(g, d, spec);
      });

    this.setupTooltip(bars, (d) =>
      TooltipFormatter.formatFields([
        [
          'Range',
          `${TooltipFormatter.formatValue(d.x0)} - ${TooltipFormatter.formatValue(d.x1)}`,
        ],
        ['Count', d.count],
      ]),
    );
  }

  private handleBarClick(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    bin: HistogramBin,
    spec: HistogramSpec,
  ) {
    // Filter by bin range
    this.selectionStrategy.onSelection(
      [{x0: bin.x0, x1: bin.x1, count: bin.count}],
      [
        {col: spec.x, op: FilterOp.Gte, val: bin.x0},
        {col: spec.x, op: FilterOp.Lt, val: bin.x1},
      ],
      {
        g,
        allData: [],
        onFilterRequest: this.onFilterRequest,
      },
    );
  }
}
