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
import {
  Row,
  ChartSpec,
  ChartType,
  LayoutMode,
  SortDirection,
  SortBy,
  FilterOp,
} from '../data/types';
import {CategoricalBrush} from './brushing';
import {TooltipFormatter} from '../tooltip';

type BarSpec = Extract<ChartSpec, {type: ChartType.Bar}>;
type GroupedBarSpec = BarSpec & {groupBy: string};

// Data structure used in stacked bar charts.
// Each datum represents a segment of a bar.
interface StackedBarDatum {
  // The original data row from the data source.
  originalRow: Row;
  // The category on the x-axis.
  category: string;
  // The group (or series) this segment belongs to.
  group: string;
  // The starting y-value of this segment.
  y0: number;
  // The ending y-value of this segment.
  y1: number;
}

export class BarRenderer extends BaseRenderer {
  constructor() {
    super();
    this.brushBehavior = new CategoricalBrush();
  }

  render(svg: SVGElement, data: Row[], spec: ChartSpec) {
    if (spec.type !== ChartType.Bar) return;

    this.clear(svg);
    const {width, height} = this.getDimensions(svg);
    const g = this.createGroup(svg);

    const sortedData = this.sortData(data, spec);

    if (spec.groupBy) {
      const mode = spec.mode ?? LayoutMode.Grouped;
      const groupedSpec = spec as GroupedBarSpec;
      if (mode === LayoutMode.Stacked) {
        this.renderStacked(g, sortedData, groupedSpec, width, height);
      } else {
        this.renderGrouped(g, sortedData, groupedSpec, width, height);
      }
    } else {
      this.renderSimple(g, sortedData, spec, width, height);
    }
  }

  private sortData(data: Row[], spec: BarSpec): Row[] {
    if (!spec.sort) {
      return data;
    }

    if (spec.groupBy && spec.mode === LayoutMode.Stacked) {
      return this.sortByStackedTotal(data, spec as GroupedBarSpec);
    }

    return this.sortByColumn(data, spec);
  }

  private sortByStackedTotal(data: Row[], spec: GroupedBarSpec): Row[] {
    const totals = new Map<string, number>();
    data.forEach((row) => {
      const category = String(row[spec.x]);
      const value = Number(row[spec.y]) || 0;
      totals.set(category, (totals.get(category) ?? 0) + value);
    });

    const sortDir = spec.sort?.direction === SortDirection.Asc ? 1 : -1;
    return [...data].sort((a, b) => {
      const totalA = totals.get(String(a[spec.x])) ?? 0;
      const totalB = totals.get(String(b[spec.x])) ?? 0;
      return (totalA - totalB) * sortDir;
    });
  }

  private sortByColumn(data: Row[], spec: BarSpec): Row[] {
    if (!spec.sort) return data;
    const {by, direction} = spec.sort;
    const column = by === SortBy.X ? spec.x : spec.y;

    return [...data].sort((a, b) => {
      const aVal = a[column];
      const bVal = b[column];
      // Handle null/undefined values
      if (aVal == null) return 1;
      if (bVal == null) return -1;
      return direction === SortDirection.Asc
        ? d3.ascending(aVal, bVal)
        : d3.descending(aVal, bVal);
    });
  }

  private renderSimple(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    data: Row[],
    spec: BarSpec,
    width: number,
    height: number,
  ) {
    const x = d3
      .scaleBand()
      .domain(data.map((d) => String(d[spec.x])))
      .range([0, width])
      .padding(0.1);

    const y = d3
      .scaleLinear()
      .domain([0, d3.max(data, (d) => Number(d[spec.y])) ?? 0])
      .nice()
      .range([height, 0]);

    this.addGridLines(g, x, y, width, height);
    this.setupBrush(g, data, spec, {x, y}, width, height);

    const bars = g
      .selectAll<SVGRectElement, Row>('.bar')
      .data(data)
      .join('rect')
      .attr('class', 'bar selectable')
      .attr('x', (d) => x(String(d[spec.x]))!)
      .attr('y', (d) => y(Number(d[spec.y])))
      .attr('width', x.bandwidth())
      .attr('height', (d) => height - y(Number(d[spec.y])))
      .attr('fill', 'steelblue')
      .style('cursor', 'pointer')
      .style('pointer-events', 'all')
      .on('click', (_event, d) => {
        const value = d[spec.x];
        if (value !== undefined && value !== null) {
          this.selectionStrategy.onSelection(
            [d],
            [{col: spec.x, op: FilterOp.Eq, val: value}],
            {
              g,
              allData: data,
              onFilterRequest: this.onFilterRequest,
            },
          );
        }
      });

    this.setupTooltip(bars, (d) =>
      TooltipFormatter.formatFields([
        [spec.x, d[spec.x]],
        [spec.y, d[spec.y]],
      ]),
    );

    this.drawAxes(g, x, y, width, height, spec.x, spec.y, undefined, (d) =>
      d3.format('.2s')(Number(d)),
    );
  }

  private renderGrouped(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    data: Row[],
    spec: GroupedBarSpec,
    width: number,
    height: number,
  ) {
    const categories = [...new Set(data.map((d) => String(d[spec.x])))];
    const groups = [...new Set(data.map((d) => String(d[spec.groupBy])))];

    const colorScale = d3
      .scaleOrdinal<string>()
      .domain(groups)
      .range(d3.schemeCategory10);

    const x0 = d3.scaleBand().domain(categories).range([0, width]).padding(0.1);
    const x1 = d3
      .scaleBand()
      .domain(groups)
      .range([0, x0.bandwidth()])
      .padding(0.05);
    const y = d3
      .scaleLinear()
      .domain([0, d3.max(data, (d) => Number(d[spec.y])) ?? 0])
      .nice()
      .range([height, 0]);

    this.addGridLines(g, x0, y, width, height);
    this.setupBrush(g, data, spec, {x: x0, y}, width, height);

    const categoryGroups = g
      .selectAll<SVGGElement, [string, Row[]]>('.category-group')
      .data(d3.group(data, (d) => String(d[spec.x])))
      .join('g')
      .attr('class', 'category-group')
      .attr('transform', ([category]) => `translate(${x0(category)},0)`);

    const bars = categoryGroups
      .selectAll<SVGRectElement, Row>('.bar')
      .data(([, rows]) => rows)
      .join('rect')
      .attr('class', 'bar selectable')
      .attr('x', (d) => x1(String(d[spec.groupBy]))!)
      .attr('y', (d) => y(Number(d[spec.y])))
      .attr('width', x1.bandwidth())
      .attr('height', (d) => height - y(Number(d[spec.y])))
      .attr('fill', (d) => colorScale(String(d[spec.groupBy])))
      .style('cursor', 'pointer')
      .style('pointer-events', 'all')
      .on('click', (_event, d) => this.handleGroupedBarClick(g, d, spec, data));

    this.setupTooltip(bars, (d) =>
      TooltipFormatter.formatFields([
        [spec.x, d[spec.x]],
        [spec.groupBy, d[spec.groupBy]],
        [spec.y, d[spec.y]],
      ]),
    );

    this.drawAxes(g, x0, y, width, height, spec.x, spec.y, undefined, (d) =>
      d3.format('.2s')(Number(d)),
    );
    this.addLegend(g, groups, colorScale, width);
  }

  private handleGroupedBarClick(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    d: Row,
    spec: GroupedBarSpec,
    allData: Row[],
  ) {
    const groupValue = d[spec.groupBy];
    const xValue = d[spec.x];
    if (groupValue == null || xValue == null) return;

    this.selectionStrategy.onSelection(
      [d],
      [
        {col: spec.groupBy, op: FilterOp.Eq, val: groupValue},
        {col: spec.x, op: FilterOp.Eq, val: xValue},
      ],
      {g, allData, onFilterRequest: this.onFilterRequest},
    );
  }

  private buildStackedData(
    data: Row[],
    spec: GroupedBarSpec,
  ): StackedBarDatum[] {
    const stackedData: StackedBarDatum[] = [];
    const totals = new Map<string, number>();

    for (const row of data) {
      const category = String(row[spec.x]);
      const group = String(row[spec.groupBy]);
      const value = Number(row[spec.y]) || 0;

      const y0 = totals.get(category) ?? 0;
      const y1 = y0 + value;
      totals.set(category, y1);

      stackedData.push({originalRow: row, category, group, y0, y1});
    }
    return stackedData;
  }

  private renderStacked(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    data: Row[],
    spec: GroupedBarSpec,
    width: number,
    height: number,
  ) {
    const categories = [...new Set(data.map((d) => String(d[spec.x])))];
    const groups = [...new Set(data.map((d) => String(d[spec.groupBy])))];
    const stackedData = this.buildStackedData(data, spec);

    const colorScale = d3
      .scaleOrdinal<string>()
      .domain(groups)
      .range(d3.schemeCategory10);

    const x = d3.scaleBand().domain(categories).range([0, width]).padding(0.1);
    const maxY = d3.max(stackedData, (d) => d.y1) ?? 0;
    const y = d3.scaleLinear().domain([0, maxY]).nice().range([height, 0]);

    this.addGridLines(g, x, y, width, height);
    this.setupBrush(
      g,
      data,
      spec,
      {x, y},
      width,
      height,
      (d) => (d as StackedBarDatum).originalRow,
    );

    const bars = g
      .selectAll<SVGRectElement, StackedBarDatum>('.bar')
      .data(stackedData)
      .join('rect')
      .attr('class', 'bar selectable')
      .attr('x', (d) => x(d.category)!)
      .attr('y', (d) => y(d.y1))
      .attr('height', (d) => y(d.y0) - y(d.y1))
      .attr('width', x.bandwidth())
      .attr('fill', (d) => colorScale(d.group))
      .style('cursor', 'pointer')
      .style('pointer-events', 'all')
      .on('click', (_event, d) =>
        this.handleGroupedBarClick(g, d.originalRow, spec, data),
      );

    this.setupTooltip(bars, (d) => {
      const value = d.y1 - d.y0;
      return TooltipFormatter.formatFields([
        [spec.x, d.category],
        [spec.groupBy, d.group],
        [spec.y, value],
      ]);
    });

    this.drawAxes(g, x, y, width, height, spec.x, spec.y, undefined, (d) =>
      d3.format('.2s')(Number(d)),
    );
    this.addLegend(g, groups, colorScale, width);
  }

  private addLegend(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    groups: string[],
    colorScale: d3.ScaleOrdinal<string, string>,
    width: number,
  ) {
    const legend = g
      .append('g')
      .attr('class', 'legend')
      .attr('transform', `translate(${width - 100}, 10)`);

    groups.forEach((group, i) => {
      const legendRow = legend
        .append('g')
        .attr('transform', `translate(0, ${i * 20})`)
        .style('cursor', 'pointer');

      legendRow
        .append('rect')
        .attr('width', 12)
        .attr('height', 12)
        .attr('fill', colorScale(group));

      legendRow
        .append('text')
        .attr('x', 18)
        .attr('y', 10)
        .style('font-size', '11px')
        .text(group);
    });
  }
}
