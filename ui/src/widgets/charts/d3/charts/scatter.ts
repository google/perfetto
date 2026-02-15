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
import {AreaBrush} from './brushing';
import {OpacitySelectionStrategy} from './selection/opacity_selection_strategy';
import {TooltipFormatter} from '../tooltip';

type ScatterSpec = Extract<ChartSpec, {type: ChartType.Scatter}>;

interface ScatterScales {
  x: d3.ScaleLinear<number, number>;
  y: d3.ScaleLinear<number, number>;
}

interface CorrelationResult {
  r: number;
  slope: number;
  intercept: number;
}

export class ScatterRenderer extends BaseRenderer {
  constructor() {
    super();
    this.brushBehavior = new AreaBrush();
    this.selectionStrategy = new OpacitySelectionStrategy();
  }

  render(svg: SVGElement, data: Row[], spec: ChartSpec) {
    if (spec.type !== ChartType.Scatter) return;

    this.clear(svg);
    const {width, height} = this.getDimensions(svg);
    const g = this.createGroup(svg);

    const {xValues, yValues} = this.extractValues(data, spec);
    const scales = this.createScales(xValues, yValues, width, height);

    if (xValues.length === 0 || yValues.length === 0) {
      this.renderEmptyState(g, width, height);
      this.setupBrush(g, data, spec, scales, width, height);
      return;
    }

    const colorScale = this.createColorScale(data, spec);

    this.addGridLines(g, scales.x, scales.y, width, height);
    this.setupBrush(g, data, spec, scales, width, height);
    this.drawPoints(g, data, spec, scales, colorScale);
    this.drawAxes(
      g,
      scales.x,
      scales.y,
      width,
      height,
      spec.x,
      spec.y,
      (d) => d3.format('.2s')(Number(d)),
      (d) => d3.format('.2s')(Number(d)),
    );

    if (spec.showCorrelation ?? true) {
      this.drawCorrelationLine(g, data, spec, scales, width);
    }

    if (spec.colorBy && colorScale) {
      this.renderLegend(
        g,
        colorScale.domain(),
        colorScale,
        width,
        (category) => {
          if (spec.colorBy) {
            this.onFilterRequest?.([
              {col: spec.colorBy, op: FilterOp.Eq, val: category},
            ]);
          }
        },
      );
    }
  }

  private extractValues(
    data: Row[],
    spec: ScatterSpec,
  ): {xValues: number[]; yValues: number[]} {
    return {
      xValues: data.map((d) => Number(d[spec.x])).filter((v) => !isNaN(v)),
      yValues: data.map((d) => Number(d[spec.y])).filter((v) => !isNaN(v)),
    };
  }

  private createScales(
    xValues: number[],
    yValues: number[],
    width: number,
    height: number,
  ): ScatterScales {
    const xDomain =
      xValues.length > 0 ? (d3.extent(xValues) as [number, number]) : [0, 100];
    const yDomain =
      yValues.length > 0 ? (d3.extent(yValues) as [number, number]) : [0, 100];

    return {
      x: d3.scaleLinear().domain(xDomain).nice().range([0, width]),
      y: d3.scaleLinear().domain(yDomain).nice().range([height, 0]),
    };
  }

  private createColorScale(
    data: Row[],
    spec: ScatterSpec,
  ): d3.ScaleOrdinal<string, string> | undefined {
    if (!spec.colorBy) return undefined;

    const categories = [...new Set(data.map((d) => String(d[spec.colorBy!])))];
    return d3
      .scaleOrdinal<string>()
      .domain(categories)
      .range(d3.schemeCategory10);
  }

  private drawPoints(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    data: Row[],
    spec: ScatterSpec,
    scales: ScatterScales,
    colorScale?: d3.ScaleOrdinal<string, string>,
  ) {
    const points = g
      .selectAll<SVGCircleElement, Row>('.point')
      .data(data)
      .join('circle')
      .attr('class', 'point selectable')
      .attr('cx', (d) => scales.x(Number(d[spec.x])))
      .attr('cy', (d) => scales.y(Number(d[spec.y])))
      .attr('r', 4)
      .attr('fill', (d) =>
        spec.colorBy && colorScale
          ? colorScale(String(d[spec.colorBy]))
          : 'steelblue',
      )
      .attr('stroke', 'var(--pf-color-background)')
      .attr('stroke-width', 0.5)
      .style('cursor', 'pointer')
      .on('click', (_event, d) => {
        this.handlePointClick(g, data, d, spec);
      });

    this.setupPointTooltips(points, spec);
  }

  private handlePointClick(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    allData: Row[],
    row: Row,
    spec: ScatterSpec,
  ) {
    const xVal = row[spec.x];
    const yVal = row[spec.y];

    if (xVal !== undefined && yVal !== undefined) {
      this.selectionStrategy.onSelection(
        [row],
        [
          {col: spec.x, op: FilterOp.Eq, val: xVal},
          {col: spec.y, op: FilterOp.Eq, val: yVal},
        ],
        {
          g,
          allData,
          onFilterRequest: this.onFilterRequest,
          updateSourceFilter: false,
        },
      );
    }
  }

  private setupPointTooltips(
    points: d3.Selection<SVGCircleElement, Row, SVGGElement, unknown>,
    spec: ScatterSpec,
  ) {
    this.setupTooltip(points, (d) => {
      const fields: [string, unknown][] = [
        [spec.x, d[spec.x]],
        [spec.y, d[spec.y]],
      ];
      if (spec.colorBy) {
        fields.push([spec.colorBy, d[spec.colorBy]]);
      }
      return TooltipFormatter.formatFields(fields);
    });
  }

  private drawCorrelationLine(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    data: Row[],
    spec: ScatterSpec,
    scales: ScatterScales,
    width: number,
  ) {
    if (data.length < 2) return;

    const {r, slope, intercept} = this.calculateCorrelation(data, spec);
    const [x1, x2] = scales.x.domain();

    g.append('line')
      .attr('class', 'correlation-line')
      .attr('x1', scales.x(x1))
      .attr('y1', scales.y(slope * x1 + intercept))
      .attr('x2', scales.x(x2))
      .attr('y2', scales.y(slope * x2 + intercept))
      .attr('stroke', 'currentColor')
      .attr('stroke-width', 1.5)
      .attr('stroke-dasharray', '5,5')
      .attr('opacity', 0.7);

    g.append('text')
      .attr('class', 'correlation-text')
      .attr('x', width - 10)
      .attr('y', 15)
      .attr('text-anchor', 'end')
      .style('font-size', '12px')
      .style('fill', 'currentColor')
      .style('font-weight', 'bold')
      .text(`r = ${r.toFixed(3)}`);
  }

  private calculateCorrelation(
    data: Row[],
    spec: ScatterSpec,
  ): CorrelationResult {
    const n = data.length;
    if (n < 2) return {r: 0, slope: 0, intercept: 0};

    const xValues = data.map((d) => Number(d[spec.x]));
    const yValues = data.map((d) => Number(d[spec.y]));

    const xMean = d3.mean(xValues) ?? 0;
    const yMean = d3.mean(yValues) ?? 0;

    const numerator = d3.sum(
      data,
      (d) => (Number(d[spec.x]) - xMean) * (Number(d[spec.y]) - yMean),
    );

    const xSumSquares = d3.sum(xValues, (x) => Math.pow(x - xMean, 2));
    const ySumSquares = d3.sum(yValues, (y) => Math.pow(y - yMean, 2));

    const denominator = Math.sqrt(xSumSquares * ySumSquares);
    const r = denominator === 0 ? 0 : numerator / denominator;

    const slope = xSumSquares === 0 ? 0 : numerator / xSumSquares;
    const intercept = yMean - slope * xMean;

    return {r, slope, intercept};
  }
}
