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
import {Row, ChartSpec, Filter, ChartType, FilterOp} from '../data/types';
import {TooltipFormatter} from '../tooltip';

type BoxplotSpec = Extract<ChartSpec, {type: ChartType.Boxplot}>;

/** Pre-aggregated boxplot stats from DataSource */
interface BoxplotStats {
  group: string | number;
  min: number;
  q1: number;
  median: number;
  q3: number;
  max: number;
}

interface BoxplotScales {
  x: d3.ScaleBand<string>;
  y: d3.ScaleLinear<number, number>;
}

export class BoxplotRenderer extends BaseRenderer {
  private stats: BoxplotStats[] = [];

  render(svg: SVGElement, data: Row[], spec: ChartSpec) {
    if (spec.type !== ChartType.Boxplot) return;

    this.clear(svg);
    const {width, height} = this.getDimensions(svg);
    const g = this.createGroup(svg);

    // Data is pre-aggregated with {group, min, q1, median, q3, max} structure
    this.stats = data.map((d) => ({
      group: String(d.group),
      min: Number(d.min),
      q1: Number(d.q1),
      median: Number(d.median),
      q3: Number(d.q3),
      max: Number(d.max),
    })) as BoxplotStats[];

    if (this.stats.length === 0) {
      this.renderEmptyState(g, width, height);
      return;
    }

    const scales = this.createScales(this.stats, width, height);

    this.addGridLines(g, scales.x, scales.y, width, height, false);
    this.setupBoxplotBrush(g, data, spec, scales, width, height, this.stats);
    this.drawBoxplots(g, this.stats, spec, scales);
    this.drawAxes(
      g,
      scales.x,
      scales.y,
      width,
      height,
      spec.x,
      spec.y,
      undefined,
      (d) => d3.format('.2s')(Number(d)),
    );
  }

  private createScales(
    stats: BoxplotStats[],
    width: number,
    height: number,
  ): BoxplotScales {
    const x = d3
      .scaleBand()
      .domain(stats.map((d) => String(d.group)))
      .range([0, width])
      .padding(0.2);

    const allValues = stats.flatMap((d) => [d.min, d.max]);
    const y = d3
      .scaleLinear()
      .domain([d3.min(allValues) ?? 0, d3.max(allValues) ?? 100])
      .range([height, 0])
      .nice();

    return {x, y};
  }

  private drawBoxplots(
    container: d3.Selection<SVGGElement, unknown, null, undefined>,
    stats: BoxplotStats[],
    spec: BoxplotSpec,
    scales: BoxplotScales,
    opacity = 1.0,
  ) {
    const boxWidth = scales.x.bandwidth();

    const groups = container
      .selectAll<SVGGElement, BoxplotStats>('.boxplot-group')
      .data(stats)
      .enter()
      .append('g')
      .attr('class', 'boxplot-group')
      .attr('transform', (d) => `translate(${scales.x(String(d.group))}, 0)`)
      .style('opacity', opacity)
      .style('pointer-events', 'all');

    this.drawWhiskers(groups, scales.y, boxWidth);
    this.drawBoxes(groups, scales.y, boxWidth);
    this.setupBoxplotTooltips(groups, spec);
  }

  private drawWhiskers(
    groups: d3.Selection<SVGGElement, BoxplotStats, SVGGElement, unknown>,
    y: d3.ScaleLinear<number, number>,
    boxWidth: number,
  ) {
    const midX = boxWidth / 2;
    const capStart = boxWidth / 4;
    const capEnd = (3 * boxWidth) / 4;

    // Lower whisker line
    groups
      .append('line')
      .attr('class', 'whisker-lower')
      .attr('x1', midX)
      .attr('x2', midX)
      .attr('y1', (d) => y(d.min))
      .attr('y2', (d) => y(d.q1))
      .attr('stroke', 'currentColor')
      .attr('stroke-width', 1);

    // Lower whisker cap
    groups
      .append('line')
      .attr('x1', capStart)
      .attr('x2', capEnd)
      .attr('y1', (d) => y(d.min))
      .attr('y2', (d) => y(d.min))
      .attr('stroke', 'currentColor')
      .attr('stroke-width', 1);

    // Upper whisker line
    groups
      .append('line')
      .attr('class', 'whisker-upper')
      .attr('x1', midX)
      .attr('x2', midX)
      .attr('y1', (d) => y(d.q3))
      .attr('y2', (d) => y(d.max))
      .attr('stroke', 'currentColor')
      .attr('stroke-width', 1);

    // Upper whisker cap
    groups
      .append('line')
      .attr('x1', capStart)
      .attr('x2', capEnd)
      .attr('y1', (d) => y(d.max))
      .attr('y2', (d) => y(d.max))
      .attr('stroke', 'currentColor')
      .attr('stroke-width', 1);
  }

  private drawBoxes(
    groups: d3.Selection<SVGGElement, BoxplotStats, SVGGElement, unknown>,
    y: d3.ScaleLinear<number, number>,
    boxWidth: number,
  ) {
    // IQR box
    groups
      .append('rect')
      .attr('class', 'box')
      .attr('x', 0)
      .attr('y', (d) => y(d.q3))
      .attr('width', boxWidth)
      .attr('height', (d) => y(d.q1) - y(d.q3))
      .attr('stroke', 'currentColor')
      .attr('stroke-width', 1)
      .attr('fill', 'steelblue')
      .attr('fill-opacity', 0.7);

    // Median line
    groups
      .append('line')
      .attr('class', 'median')
      .attr('x1', 0)
      .attr('x2', boxWidth)
      .attr('y1', (d) => y(d.median))
      .attr('y2', (d) => y(d.median))
      .attr('stroke', 'currentColor')
      .attr('stroke-width', 2);
  }

  private setupBoxplotTooltips(
    groups: d3.Selection<SVGGElement, BoxplotStats, SVGGElement, unknown>,
    spec: BoxplotSpec,
  ) {
    this.setupTooltip(groups, (d: BoxplotStats) =>
      TooltipFormatter.formatFields([
        [spec.x, String(d.group)],
        ['Max', d.max],
        ['Q3', d.q3],
        ['Median', d.median],
        ['Q1', d.q1],
        ['Min', d.min],
      ]),
    );
  }

  protected setupBoxplotBrush(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    data: Row[],
    spec: BoxplotSpec,
    scales: BoxplotScales,
    width: number,
    height: number,
    stats: BoxplotStats[],
  ) {
    this.setup2DBrush(
      g,
      width,
      height,
      data,
      (selection) => {
        this.handleBrushSelection(g, selection, stats, data, spec, scales);
      },
      () => this.clearBrushVisuals(g, 'boxplot'),
    );
  }

  private handleBrushSelection(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    selection: [[number, number], [number, number]],
    stats: BoxplotStats[],
    data: Row[],
    spec: BoxplotSpec,
    scales: BoxplotScales,
  ) {
    const [[x0, y0], [x1, y1]] = selection;
    const minY = scales.y.invert(y1);
    const maxY = scales.y.invert(y0);

    const selectedCategories = this.getSelectedCategories(
      stats,
      scales.x,
      x0,
      x1,
    );

    this.clearBrushVisuals(g, 'boxplot');

    if (this.selectionStrategy?.usesClipPaths()) {
      this.renderClippedBoxplots(g, stats, spec, scales, x0, y0, x1, y1);
    } else {
      g.selectAll('.boxplot-group').style('opacity', 0.7);
    }

    const filters = this.createFilters(
      spec,
      minY,
      maxY,
      selectedCategories,
      stats.length,
    );

    this.selectionStrategy?.onSelection([], filters, {
      g,
      allData: data,
      onFilterRequest: this.onFilterRequest,
    });
  }

  private getSelectedCategories(
    stats: BoxplotStats[],
    xScale: d3.ScaleBand<string>,
    x0: number,
    x1: number,
  ): string[] {
    const bandwidth = xScale.bandwidth();

    return stats
      .filter((d) => {
        const categoryX = xScale(String(d.group));
        return (
          categoryX !== undefined &&
          x0 < categoryX + bandwidth &&
          x1 > categoryX
        );
      })
      .map((d) => String(d.group));
  }

  private renderClippedBoxplots(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    stats: BoxplotStats[],
    spec: BoxplotSpec,
    scales: BoxplotScales,
    x0: number,
    y0: number,
    x1: number,
    y1: number,
  ) {
    g.selectAll('.boxplot-group').style('opacity', 0);

    // Dimmed layer
    this.drawBoxplots(
      g.append('g').attr('class', 'boxplot-dimmed'),
      stats,
      spec,
      scales,
      0.2,
    );

    // Highlighted layer with clip
    if (this.clipPaths) {
      const clipUrl = this.clipPaths.createRectClip(x0, y0, x1 - x0, y1 - y0);

      this.drawBoxplots(
        g
          .append('g')
          .attr('class', 'boxplot-highlight')
          .attr('clip-path', clipUrl),
        stats,
        spec,
        scales,
        1.0,
      );
    }
  }

  private createFilters(
    spec: BoxplotSpec,
    minY: number,
    maxY: number,
    selectedCategories: string[],
    totalCategories: number,
  ): Filter[] {
    const filters: Filter[] = [
      {col: spec.y, op: FilterOp.Gte, val: minY},
      {col: spec.y, op: FilterOp.Lte, val: maxY},
    ];

    if (
      selectedCategories.length > 0 &&
      selectedCategories.length < totalCategories
    ) {
      filters.push({col: spec.x, op: FilterOp.In, val: selectedCategories});
    }

    return filters;
  }
}
