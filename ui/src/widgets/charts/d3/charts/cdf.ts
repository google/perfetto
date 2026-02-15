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
import {Tooltip, TooltipFormatter} from '../tooltip';

type CDFSpec = Extract<ChartSpec, {type: ChartType.Cdf}>;

const DEFAULT_GROUP = 'default';

interface CDFPoint {
  x: number;
  y: number;
}

interface TooltipDatum {
  key: string;
  y: number;
  color: string;
}

interface CDFScales {
  x: d3.ScaleLinear<number, number>;
  y: d3.ScaleLinear<number, number>;
}

interface CrosshairElements {
  crosshairGroup: d3.Selection<SVGGElement, unknown, null, undefined>;
  horizontalLinesGroup: d3.Selection<SVGGElement, unknown, null, undefined>;
  dotsGroup: d3.Selection<SVGGElement, unknown, null, undefined>;
}

type CDFLine = d3.Line<CDFPoint>;
type ColorFn = (key: string) => string;

export class CDFRenderer extends BaseRenderer {
  constructor() {
    super();
    this.brushBehavior = new RangeBrush();
  }

  render(svg: SVGElement, data: Row[], spec: ChartSpec) {
    if (spec.type !== ChartType.Cdf) return;

    this.clear(svg);
    const {width, height} = this.getDimensions(svg);
    const g = this.createGroup(svg);

    const {cdfByGroup, allXValues, categories} = this.prepareCDFData(
      data,
      spec,
    );

    if (allXValues.length === 0) {
      this.renderEmpty(g, cdfByGroup, spec, width, height);
      return;
    }

    const scales = this.createScales(allXValues, width, height);
    const getColor = this.createColorFn(categories, spec);
    const line = this.createLine(scales);

    this.renderChart(
      g,
      cdfByGroup,
      spec,
      scales,
      width,
      height,
      line,
      getColor,
      categories,
    );
  }

  private prepareCDFData(
    data: Row[],
    spec: CDFSpec,
  ): {
    cdfByGroup: Map<string, CDFPoint[]>;
    allXValues: number[];
    categories: string[];
  } {
    // Normalize both grouped and ungrouped CDF data into Map<string, Row[]>.
    // Ungrouped CDFs get a synthetic DEFAULT_GROUP key. This eliminates branching
    // in downstream rendering (paths, crosshairs, tooltips all loop over groups).
    const grouped = spec.colorBy
      ? d3.group(data, (d) => String(d.group ?? DEFAULT_GROUP))
      : new Map([[DEFAULT_GROUP, data]]);

    // Extract category names for legend, excluding the synthetic default group.
    const categories = spec.colorBy
      ? Array.from(grouped.keys()).filter((k) => k !== DEFAULT_GROUP)
      : [];

    const cdfByGroup = new Map<string, CDFPoint[]>();
    const allXValues: number[] = [];

    for (const [key, rows] of grouped) {
      const points = rows.map((d) => ({
        x: Number(d.value),
        y: Number(d.probability),
      }));

      cdfByGroup.set(key, points);
      for (const p of points) {
        allXValues.push(p.x);
      }
    }

    return {cdfByGroup, allXValues, categories};
  }

  private createScales(
    allXValues: number[],
    width: number,
    height: number,
  ): CDFScales {
    const xExtent = d3.extent(allXValues) as [number, number];
    return {
      x: d3.scaleLinear().domain(xExtent).range([0, width]),
      y: d3.scaleLinear().domain([0, 1]).range([height, 0]),
    };
  }

  private createColorFn(categories: string[], spec: CDFSpec): ColorFn {
    const colorScale = d3
      .scaleOrdinal<string>()
      .domain(categories)
      .range(d3.schemeCategory10);

    return (key: string) => (spec.colorBy ? colorScale(key) : 'steelblue');
  }

  private createLine(scales: CDFScales): CDFLine {
    return d3
      .line<CDFPoint>()
      .x((d) => scales.x(d.x))
      .y((d) => scales.y(d.y));
  }

  private renderEmpty(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    cdfByGroup: Map<string, CDFPoint[]>,
    spec: CDFSpec,
    width: number,
    height: number,
  ) {
    this.renderEmptyState(g, width, height);

    const dummyScales: CDFScales = {
      x: d3.scaleLinear().domain([0, 100]).range([0, width]),
      y: d3.scaleLinear().domain([0, 1]).range([height, 0]),
    };
    const dummyLine = d3.line<CDFPoint>();

    this.setupCDFBrush(
      g,
      cdfByGroup,
      spec,
      dummyScales,
      width,
      height,
      dummyLine,
      () => 'steelblue',
    );
  }

  private renderChart(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    cdfByGroup: Map<string, CDFPoint[]>,
    spec: CDFSpec,
    scales: CDFScales,
    width: number,
    height: number,
    line: CDFLine,
    getColor: ColorFn,
    categories: string[],
  ) {
    this.addGridLines(g, scales.x, scales.y, width, height);
    this.drawAxes(
      g,
      scales.x,
      scales.y,
      width,
      height,
      spec.x,
      'Cumulative Probability',
      (d) => d3.format('.2s')(Number(d)),
      (d) => `${(Number(d) * 100).toFixed(0)}%`,
    );

    this.renderLines(g, cdfByGroup, spec, line, getColor);
    this.setupCDFBrush(
      g,
      cdfByGroup,
      spec,
      scales,
      width,
      height,
      line,
      getColor,
    );
    this.setupCrosshair(g, cdfByGroup, spec, scales, width, getColor);

    if (spec.colorBy) {
      const colorScale = d3
        .scaleOrdinal<string>()
        .domain(categories)
        .range(d3.schemeCategory10);

      this.renderLegend(g, categories, colorScale, width, (category) => {
        this.onFilterRequest?.([
          {col: spec.colorBy!, op: FilterOp.Eq, val: category},
        ]);
      });
    }
  }

  private renderLines(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    cdfByGroup: Map<string, CDFPoint[]>,
    spec: CDFSpec,
    line: CDFLine,
    getColor: ColorFn,
  ) {
    for (const [key, points] of cdfByGroup) {
      const path = g
        .append('path')
        .datum(points)
        .attr('class', 'cdf-line')
        .attr('fill', 'none')
        .attr('stroke', getColor(key))
        .attr('stroke-width', 2)
        .attr('d', line)
        .style('pointer-events', 'stroke')
        .style('cursor', spec.colorBy ? 'pointer' : 'default');

      if (spec.colorBy) {
        path.on('click', () => {
          this.onFilterRequest?.([
            {col: spec.colorBy!, op: FilterOp.Eq, val: key},
          ]);
        });
      }
    }
  }

  private setupCDFBrush(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    cdfByGroup: Map<string, CDFPoint[]>,
    spec: CDFSpec,
    scales: CDFScales,
    width: number,
    height: number,
    line: CDFLine,
    getColor: ColorFn,
  ) {
    const brush = d3
      .brushX()
      .extent([
        [0, 0],
        [width, height],
      ])
      .on('end', (event: d3.D3BrushEvent<unknown>) => {
        this.handleBrushEnd(
          event,
          g,
          cdfByGroup,
          spec,
          scales,
          height,
          line,
          getColor,
        );
      });

    g.append('g').attr('class', 'brush').call(brush);
  }

  private handleBrushEnd(
    event: d3.D3BrushEvent<unknown>,
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    cdfByGroup: Map<string, CDFPoint[]>,
    spec: CDFSpec,
    scales: CDFScales,
    height: number,
    line: CDFLine,
    getColor: ColorFn,
  ) {
    if (event.selection === null) {
      this.clearBrushVisuals(g, 'cdf');
      this.selectionStrategy?.onClear({
        g,
        allData: [],
        onFilterRequest: this.onFilterRequest,
      });
      return;
    }

    const [x0, x1] = event.selection as [number, number];
    const minValue = scales.x.invert(x0);
    const maxValue = scales.x.invert(x1);

    this.clearBrushVisuals(g, 'cdf');

    if (this.selectionStrategy?.usesClipPaths()) {
      this.renderClippedLines(g, cdfByGroup, line, getColor, x0, x1, height);
    } else {
      g.selectAll('.cdf-line').style('opacity', 0.7);
    }

    this.selectionStrategy?.onSelection(
      [],
      [
        {col: spec.x, op: FilterOp.Gte, val: minValue},
        {col: spec.x, op: FilterOp.Lte, val: maxValue},
      ],
      {g, allData: [], onFilterRequest: this.onFilterRequest},
    );
  }

  private renderClippedLines(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    cdfByGroup: Map<string, CDFPoint[]>,
    line: CDFLine,
    getColor: ColorFn,
    x0: number,
    x1: number,
    height: number,
  ) {
    g.selectAll('.cdf-line').style('opacity', 0);

    const clipUrl =
      this.clipPaths?.createRectClip(x0, 0, x1 - x0, height) ?? '';

    for (const [key, points] of cdfByGroup) {
      const color = getColor(key);

      g.append('path')
        .datum(points)
        .attr('class', 'cdf-dimmed')
        .attr('fill', 'none')
        .attr('stroke', color)
        .attr('stroke-width', 2)
        .attr('d', line)
        .style('opacity', 0.2)
        .style('pointer-events', 'none');

      if (clipUrl) {
        g.append('path')
          .datum(points)
          .attr('class', 'cdf-highlight')
          .attr('fill', 'none')
          .attr('stroke', color)
          .attr('stroke-width', 2)
          .attr('d', line)
          .attr('clip-path', clipUrl)
          .style('pointer-events', 'none');
      }
    }
  }

  private setupCrosshair(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    cdfByGroup: Map<string, CDFPoint[]>,
    spec: CDFSpec,
    scales: CDFScales,
    width: number,
    getColor: ColorFn,
  ) {
    const elements = this.createCrosshairElements(g, scales.y.range()[0]);
    const tooltip = Tooltip.getInstance();

    g.select('.brush')
      .on('mouseover', () => elements.crosshairGroup.style('display', null))
      .on('mouseout', () => {
        tooltip.hide();
        elements.crosshairGroup.style('display', 'none');
      })
      .on('mousemove', (event: MouseEvent) => {
        this.updateCrosshair(
          event,
          cdfByGroup,
          spec,
          scales,
          width,
          elements,
          getColor,
          tooltip,
        );
      });
  }

  private updateCrosshair(
    event: MouseEvent,
    cdfByGroup: Map<string, CDFPoint[]>,
    spec: CDFSpec,
    scales: CDFScales,
    width: number,
    elements: CrosshairElements,
    getColor: ColorFn,
    tooltip: Tooltip,
  ) {
    if (cdfByGroup.size === 0) return;

    const [mouseX] = d3.pointer(event);
    const xValue = scales.x.invert(mouseX);
    const crosshairX = scales.x(xValue);

    elements.crosshairGroup
      .select('.crosshair-vertical')
      .attr('x1', crosshairX)
      .attr('x2', crosshairX);

    const tooltipData = this.interpolateCDFValues(cdfByGroup, xValue, getColor);

    this.renderHorizontalLines(
      elements.horizontalLinesGroup,
      tooltipData,
      scales.y,
      width,
    );
    this.renderCrosshairDots(
      elements.dotsGroup,
      tooltipData,
      crosshairX,
      scales.y,
    );

    tooltip.show(
      this.formatTooltip(spec.x, xValue, tooltipData),
      event.pageX,
      event.pageY,
    );
  }

  private interpolateCDFValues(
    cdfByGroup: Map<string, CDFPoint[]>,
    xValue: number,
    getColor: ColorFn,
  ): TooltipDatum[] {
    const bisect = d3.bisector((d: CDFPoint) => d.x).left;
    const results: TooltipDatum[] = [];

    for (const [key, points] of cdfByGroup) {
      const i = bisect(points, xValue);
      let y: number;

      if (i === 0) {
        y = points[0].y;
      } else if (i >= points.length) {
        y = points[points.length - 1].y;
      } else {
        const p0 = points[i - 1];
        const p1 = points[i];
        const t = (xValue - p0.x) / (p1.x - p0.x);
        y = p0.y + t * (p1.y - p0.y);
      }

      results.push({key, y, color: getColor(key)});
    }

    return results;
  }

  private renderHorizontalLines(
    group: d3.Selection<SVGGElement, unknown, null, undefined>,
    data: TooltipDatum[],
    yScale: d3.ScaleLinear<number, number>,
    width: number,
  ) {
    group
      .selectAll<SVGLineElement, TooltipDatum>('line')
      .data(data)
      .join('line')
      .attr('x1', 0)
      .attr('x2', width)
      .attr('y1', (d) => yScale(d.y))
      .attr('y2', (d) => yScale(d.y))
      .attr('stroke', (d) => d.color)
      .attr('stroke-width', 1)
      .attr('stroke-dasharray', '4,4')
      .attr('opacity', 0.7);
  }

  private renderCrosshairDots(
    group: d3.Selection<SVGGElement, unknown, null, undefined>,
    data: TooltipDatum[],
    x: number,
    yScale: d3.ScaleLinear<number, number>,
  ) {
    group
      .selectAll<SVGCircleElement, TooltipDatum>('circle')
      .data(data)
      .join('circle')
      .attr('cx', x)
      .attr('cy', (d) => yScale(d.y))
      .attr('r', 4)
      .attr('fill', (d) => d.color)
      .attr('stroke', 'white')
      .attr('stroke-width', 2);
  }

  private formatTooltip(
    xLabel: string,
    xValue: number,
    data: TooltipDatum[],
  ): string {
    const sorted = [...data].sort((a, b) => b.y - a.y);
    const lines = sorted.map((d) => {
      const label = d.key === DEFAULT_GROUP ? 'CDF' : d.key;
      return `<span style="color: ${d.color}">●</span> <strong>${label}:</strong> ${(d.y * 100).toFixed(1)}%`;
    });

    return `${TooltipFormatter.formatField(xLabel, xValue)}<br/>${lines.join('<br/>')}`;
  }
}
