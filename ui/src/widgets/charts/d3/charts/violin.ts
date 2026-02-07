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

/** Bandwidth parameter for Epanechnikov kernel density estimation */
export const DEFAULT_KDE_BANDWIDTH = 0.5;

/** Number of sample points for kernel density estimation curves */
export const DEFAULT_KDE_SAMPLE_POINTS = 50;

/** Constant for the Epanechnikov kernel calculation. */
export const EPANECHNIKOV_KERNEL_CONST = 0.75;

type ViolinSpec = Extract<ChartSpec, {type: ChartType.Violin}>;

/** Pre-aggregated violin stats from DataSource */
interface ViolinStats {
  group: string | number;
  density: [number, number][]; // [value, density] pairs
  min: number;
  max: number;
  q1: number;
  median: number;
  q3: number;
  p90: number;
  p95: number;
  p99: number;
}

interface ViolinScales {
  x: d3.ScaleBand<string>;
  y: d3.ScaleLinear<number, number>;
}

export class ViolinRenderer extends BaseRenderer {
  render(svg: SVGElement, data: Row[], spec: ChartSpec) {
    if (spec.type !== ChartType.Violin) return;

    this.clear(svg);
    const {width, height} = this.getDimensions(svg);
    const g = this.createGroup(svg);

    // Data is pre-aggregated with {group, density, min, max, q1, median, q3, p90, p95, p99}
    const stats = data.map((d) => ({
      group: d.group as string | number,
      density: JSON.parse(d.density as string) as [number, number][],
      min: Number(d.min),
      max: Number(d.max),
      q1: Number(d.q1),
      median: Number(d.median),
      q3: Number(d.q3),
      p90: Number(d.p90),
      p95: Number(d.p95),
      p99: Number(d.p99),
    })) as ViolinStats[];

    if (stats.length === 0) {
      this.renderEmptyState(g, width, height);
      return;
    }

    const scales = this.createScales(stats, width, height);

    this.addGridLines(g, scales.x, scales.y, width, height, false);
    this.setupViolinBrush(g, stats, data, spec, scales, width, height);
    this.drawViolins(g, stats, spec, scales);
    this.drawAxes(g, scales.x, scales.y, width, height, spec.x, spec.y);
  }

  private createScales(
    stats: ViolinStats[],
    width: number,
    height: number,
  ): ViolinScales {
    const allYValues: number[] = [];
    for (const d of stats) {
      allYValues.push(d.min, d.max, d.q1, d.median, d.q3, d.p90, d.p95, d.p99);
      for (const p of d.density) {
        allYValues.push(p[0]);
      }
    }

    return {
      x: d3
        .scaleBand()
        .domain(stats.map((d) => String(d.group)))
        .range([0, width])
        .padding(0.1),
      y: d3
        .scaleLinear()
        .domain([d3.min(allYValues) ?? 0, d3.max(allYValues) ?? 100])
        .nice()
        .range([height, 0]),
    };
  }

  private drawViolins(
    container: d3.Selection<SVGGElement, unknown, null, undefined>,
    stats: ViolinStats[],
    spec: ViolinSpec,
    scales: ViolinScales,
    opacity = 1.0,
  ) {
    const maxDensity =
      d3.max(stats, (d) => d3.max(d.density, (p) => p[1])) ?? 1;

    const densityScale = d3
      .scaleLinear()
      .domain([0, maxDensity])
      .range([0, scales.x.bandwidth() / 2]);

    const area = d3
      .area<[number, number]>()
      .x0((d) => -densityScale(d[1]))
      .x1((d) => densityScale(d[1]))
      .y((d) => scales.y(d[0]))
      .curve(d3.curveCatmullRom);

    const groups = container
      .selectAll<SVGGElement, ViolinStats>('.violin-group')
      .data(stats)
      .enter()
      .append('g')
      .attr('class', 'violin-group')
      .attr(
        'transform',
        (d) =>
          `translate(${(scales.x(String(d.group)) ?? 0) + scales.x.bandwidth() / 2}, 0)`,
      )
      .style('opacity', opacity)
      .style('pointer-events', 'all');

    this.drawViolinShapes(groups, area);
    this.drawViolinStats(groups, scales.y);
    this.setupViolinTooltips(groups, spec);
  }

  private drawViolinShapes(
    groups: d3.Selection<SVGGElement, ViolinStats, SVGGElement, unknown>,
    area: d3.Area<[number, number]>,
  ) {
    groups
      .append('path')
      .datum((d) => d.density)
      .attr('d', area)
      .style('fill', 'steelblue')
      .style('opacity', 0.7);
  }

  private drawViolinStats(
    groups: d3.Selection<SVGGElement, ViolinStats, SVGGElement, unknown>,
    y: d3.ScaleLinear<number, number>,
  ) {
    // IQR line
    groups
      .append('line')
      .attr('x1', 0)
      .attr('x2', 0)
      .attr('y1', (d) => y(d.q1))
      .attr('y2', (d) => y(d.q3))
      .attr('stroke', 'currentColor')
      .style('stroke-width', 2);

    // Median dot
    this.drawStatDot(
      groups,
      y,
      (d) => d.median,
      'var(--pf-color-background)',
      'currentColor',
    );

    // Percentile dots
    this.drawStatDot(groups, y, (d) => d.p90, 'orange');
    this.drawStatDot(groups, y, (d) => d.p95, 'red');
    this.drawStatDot(groups, y, (d) => d.p99, 'purple');
  }

  private drawStatDot(
    groups: d3.Selection<SVGGElement, ViolinStats, SVGGElement, unknown>,
    y: d3.ScaleLinear<number, number>,
    valueFn: (d: ViolinStats) => number,
    fill: string,
    stroke?: string,
  ) {
    const dot = groups
      .append('circle')
      .attr('cx', 0)
      .attr('cy', (d) => y(valueFn(d)))
      .attr('r', 3)
      .style('fill', fill);

    if (stroke) {
      dot.style('stroke', stroke);
    }
  }

  private setupViolinTooltips(
    groups: d3.Selection<SVGGElement, ViolinStats, SVGGElement, unknown>,
    spec: ViolinSpec,
  ) {
    this.setupTooltip(groups, (d: ViolinStats) =>
      TooltipFormatter.formatFields([
        [spec.x, String(d.group)],
        ['Median', d.median],
        ['Q1', d.q1],
        ['Q3', d.q3],
        ['P90', d.p90],
        ['P95', d.p95],
        ['P99', d.p99],
      ]),
    );
  }

  private setupViolinBrush(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    stats: ViolinStats[],
    data: Row[],
    spec: ViolinSpec,
    scales: ViolinScales,
    width: number,
    height: number,
  ) {
    this.setup2DBrush(
      g,
      width,
      height,
      data,
      (selection) => {
        this.handleBrushSelection(g, selection, stats, data, spec, scales);
      },
      () => this.clearBrushVisuals(g, 'violin'),
    );
  }

  private handleBrushSelection(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    selection: [[number, number], [number, number]],
    stats: ViolinStats[],
    data: Row[],
    spec: ViolinSpec,
    scales: ViolinScales,
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

    this.clearBrushVisuals(g, 'violin');

    if (this.selectionStrategy?.usesClipPaths()) {
      this.renderClippedViolins(g, stats, spec, scales, x0, y0, x1, y1);
    } else {
      g.selectAll('.violin-group').style('opacity', 0.7);
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
    stats: ViolinStats[],
    xScale: d3.ScaleBand<string>,
    x0: number,
    x1: number,
  ): string[] {
    const bandwidth = xScale.bandwidth();

    return stats
      .filter((d) => {
        const pos = xScale(String(d.group));
        return pos !== undefined && x0 < pos + bandwidth && x1 > pos;
      })
      .map((d) => String(d.group));
  }

  private renderClippedViolins(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    stats: ViolinStats[],
    spec: ViolinSpec,
    scales: ViolinScales,
    x0: number,
    y0: number,
    x1: number,
    y1: number,
  ) {
    g.selectAll('.violin-group').style('opacity', 0);

    // Dimmed layer
    this.drawViolins(
      g.append('g').attr('class', 'violin-dimmed'),
      stats,
      spec,
      scales,
      0.2,
    );

    // Highlighted layer with clip
    if (this.clipPaths) {
      const clipUrl = this.clipPaths.createRectClip(x0, y0, x1 - x0, y1 - y0);

      this.drawViolins(
        g
          .append('g')
          .attr('class', 'violin-highlight')
          .attr('clip-path', clipUrl),
        stats,
        spec,
        scales,
        1.0,
      );
    }
  }

  private createFilters(
    spec: ViolinSpec,
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
