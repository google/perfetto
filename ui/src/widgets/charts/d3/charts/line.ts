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
import {Tooltip, TooltipFormatter} from '../tooltip';

type LineSpec = Extract<ChartSpec, {type: ChartType.Line}>;

const DEFAULT_GROUP = 'default';

interface Point {
  x: number;
  y: number;
}

interface TooltipDatum {
  key: string;
  y: number;
  color: string;
}

interface LineScales {
  x: d3.ScaleLinear<number, number>;
  y: d3.ScaleLinear<number, number>;
}

interface CrosshairElements {
  crosshairGroup: d3.Selection<SVGGElement, unknown, null, undefined>;
  horizontalLinesGroup: d3.Selection<SVGGElement, unknown, null, undefined>;
  dotsGroup: d3.Selection<SVGGElement, unknown, null, undefined>;
}

type LineFn = d3.Line<Point>;
type ColorFn = (key: string) => string;

export class LineRenderer extends BaseRenderer {
  render(svg: SVGElement, data: Row[], spec: ChartSpec) {
    if (spec.type !== ChartType.Line) return;

    this.clear(svg);
    const {width, height} = this.getDimensions(svg);
    const g = this.createGroup(svg);

    const {pointsByGroup, allX, allY, categories} = this.prepareData(
      data,
      spec,
    );

    if (pointsByGroup.size === 0) {
      this.renderEmpty(g, pointsByGroup, spec, width, height);
      return;
    }

    const scales = this.createScales(allX, allY, width, height);
    const getColor = this.createColorFn(categories, spec);
    const line = this.createLine(scales);

    this.renderChart(
      g,
      pointsByGroup,
      spec,
      scales,
      width,
      height,
      line,
      getColor,
      categories,
    );
  }

  private prepareData(
    data: Row[],
    spec: LineSpec,
  ): {
    pointsByGroup: Map<string, Point[]>;
    allX: number[];
    allY: number[];
    categories: string[];
  } {
    // Normalize both grouped and ungrouped line data into Map<string, Row[]>.
    // Ungrouped lines get a synthetic DEFAULT_GROUP key. This eliminates branching
    // in downstream rendering (paths, crosshairs, tooltips all loop over groups).
    const grouped = spec.colorBy
      ? d3.group(data, (d) => String(d[spec.colorBy!]))
      : new Map([[DEFAULT_GROUP, data]]);

    // Extract category names for legend, excluding the synthetic default group.
    const categories = spec.colorBy
      ? Array.from(grouped.keys()).filter((k) => k !== DEFAULT_GROUP)
      : [];

    const pointsByGroup = new Map<string, Point[]>();
    const allX: number[] = [];
    const allY: number[] = [];

    for (const [key, rows] of grouped) {
      const points = rows
        .map((d) => ({
          x: Number(d[spec.x]),
          y: Number(d[spec.y]),
        }))
        .filter((p) => !isNaN(p.x) && !isNaN(p.y))
        .sort((a, b) => a.x - b.x);

      if (points.length > 0) {
        pointsByGroup.set(key, points);
        for (const p of points) {
          allX.push(p.x);
          allY.push(p.y);
        }
      }
    }

    return {pointsByGroup, allX, allY, categories};
  }

  private createScales(
    allX: number[],
    allY: number[],
    width: number,
    height: number,
  ): LineScales {
    return {
      x: d3
        .scaleLinear()
        .domain(d3.extent(allX) as [number, number])
        .range([0, width]),
      y: d3
        .scaleLinear()
        .domain(d3.extent(allY) as [number, number])
        .nice()
        .range([height, 0]),
    };
  }

  private createColorFn(categories: string[], spec: LineSpec): ColorFn {
    const colorScale = d3
      .scaleOrdinal<string>()
      .domain(categories)
      .range(d3.schemeCategory10);

    return (key: string) => (spec.colorBy ? colorScale(key) : 'steelblue');
  }

  private createLine(scales: LineScales): LineFn {
    return d3
      .line<Point>()
      .x((d) => scales.x(d.x))
      .y((d) => scales.y(d.y));
  }

  private renderEmpty(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    pointsByGroup: Map<string, Point[]>,
    spec: LineSpec,
    width: number,
    height: number,
  ) {
    this.renderEmptyState(g, width, height);

    const dummyScales: LineScales = {
      x: d3.scaleLinear().domain([0, 100]).range([0, width]),
      y: d3.scaleLinear().domain([0, 100]).range([height, 0]),
    };

    this.setupLineBrush(
      g,
      pointsByGroup,
      spec,
      dummyScales,
      width,
      height,
      d3.line(),
      () => 'steelblue',
    );
  }

  private renderChart(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    pointsByGroup: Map<string, Point[]>,
    spec: LineSpec,
    scales: LineScales,
    width: number,
    height: number,
    line: LineFn,
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
      spec.y,
      (d) => d3.format('.2s')(Number(d)),
      (d) => d3.format('.2s')(Number(d)),
    );

    this.renderLines(g, pointsByGroup, spec, line, getColor);
    this.setupLineBrush(
      g,
      pointsByGroup,
      spec,
      scales,
      width,
      height,
      line,
      getColor,
    );
    this.setupCrosshair(g, pointsByGroup, spec, scales, width, getColor);

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
    pointsByGroup: Map<string, Point[]>,
    spec: LineSpec,
    line: LineFn,
    getColor: ColorFn,
  ) {
    for (const [key, points] of pointsByGroup) {
      const path = g
        .append('path')
        .datum(points)
        .attr('class', 'line-path')
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

  private setupLineBrush(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    pointsByGroup: Map<string, Point[]>,
    spec: LineSpec,
    scales: LineScales,
    width: number,
    height: number,
    line: LineFn,
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
          pointsByGroup,
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
    pointsByGroup: Map<string, Point[]>,
    spec: LineSpec,
    scales: LineScales,
    height: number,
    line: LineFn,
    getColor: ColorFn,
  ) {
    if (event.selection === null) {
      this.clearBrushVisuals(g, 'line');
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

    this.clearBrushVisuals(g, 'line');

    if (this.selectionStrategy?.usesClipPaths()) {
      this.renderClippedLines(g, pointsByGroup, line, getColor, x0, x1, height);
    } else {
      g.selectAll('.line-path').style('opacity', 0.7);
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
    pointsByGroup: Map<string, Point[]>,
    line: LineFn,
    getColor: ColorFn,
    x0: number,
    x1: number,
    height: number,
  ) {
    g.selectAll('.line-path').style('opacity', 0);

    const clipUrl =
      this.clipPaths?.createRectClip(x0, 0, x1 - x0, height) ?? '';

    for (const [key, points] of pointsByGroup) {
      const color = getColor(key);

      g.append('path')
        .datum(points)
        .attr('class', 'line-dimmed')
        .attr('fill', 'none')
        .attr('stroke', color)
        .attr('stroke-width', 2)
        .attr('d', line)
        .style('opacity', 0.2)
        .style('pointer-events', 'none');

      if (clipUrl) {
        g.append('path')
          .datum(points)
          .attr('class', 'line-highlight')
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
    pointsByGroup: Map<string, Point[]>,
    spec: LineSpec,
    scales: LineScales,
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
          pointsByGroup,
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
    pointsByGroup: Map<string, Point[]>,
    spec: LineSpec,
    scales: LineScales,
    width: number,
    elements: CrosshairElements,
    getColor: ColorFn,
    tooltip: Tooltip,
  ) {
    if (pointsByGroup.size === 0) return;

    const [mouseX] = d3.pointer(event);
    const xValue = scales.x.invert(mouseX);
    const crosshairX = scales.x(xValue);

    elements.crosshairGroup
      .select('.crosshair-vertical')
      .attr('x1', crosshairX)
      .attr('x2', crosshairX);

    const tooltipData = this.interpolateValues(pointsByGroup, xValue, getColor);

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

  private interpolateValues(
    pointsByGroup: Map<string, Point[]>,
    xValue: number,
    getColor: ColorFn,
  ): TooltipDatum[] {
    const bisect = d3.bisector((d: Point) => d.x).left;
    const results: TooltipDatum[] = [];

    for (const [key, points] of pointsByGroup) {
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
      const label = d.key === DEFAULT_GROUP ? 'Value' : d.key;
      return `<span style="color: ${d.color}">●</span> <strong>${label}:</strong> ${TooltipFormatter.formatValue(d.y)}`;
    });

    return `${TooltipFormatter.formatField(xLabel, xValue)}<br/>${lines.join('<br/>')}`;
  }
}
