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
import {TooltipFormatter} from '../tooltip';

type DonutSpec = Extract<ChartSpec, {type: ChartType.Donut}>;

interface PieSlice extends d3.PieArcDatum<Row> {
  data: Row;
}

type SliceSelection = d3.Selection<
  SVGPathElement,
  PieSlice,
  SVGGElement,
  unknown
>;

const LEGEND_WIDTH = 120;
const INNER_RADIUS_RATIO = 0.5;

export class DonutChartRenderer extends BaseRenderer {
  private selectedSlices = new Set<string>();

  render(svg: SVGElement, data: Row[], spec: ChartSpec) {
    if (spec.type !== ChartType.Donut) return;

    this.clear(svg);
    const {width, height} = this.getDimensions(svg);
    const g = this.createGroup(svg);

    if (data.length === 0) {
      this.renderEmptyState(g, width, height);
      return;
    }

    const chartWidth = width - LEGEND_WIDTH;
    const radius = Math.min(chartWidth, height) / 2 - 10;
    const innerRadius = radius * INNER_RADIUS_RATIO;
    const centerX = chartWidth / 2;
    const centerY = height / 2;

    const colorScale = this.createColorScale(data, spec);
    const slices = this.drawSlices(
      g,
      data,
      spec,
      colorScale,
      centerX,
      centerY,
      innerRadius,
      radius,
    );

    this.setupSliceInteraction(svg, slices, spec);
    this.setupSliceTooltips(slices, spec);
    this.drawLegend(g, colorScale, chartWidth, slices, spec);
  }

  private createColorScale(
    data: Row[],
    spec: DonutSpec,
  ): d3.ScaleOrdinal<string, string> {
    const categories = data.map((d) => String(d[spec.category]));
    return d3
      .scaleOrdinal<string>()
      .domain(categories)
      .range(d3.schemeTableau10);
  }

  private drawSlices(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    data: Row[],
    spec: DonutSpec,
    colorScale: d3.ScaleOrdinal<string, string>,
    centerX: number,
    centerY: number,
    innerRadius: number,
    outerRadius: number,
  ): SliceSelection {
    const pie = d3
      .pie<Row>()
      .value((d) => Number(d[spec.value]))
      .sort(null);

    const arc = d3
      .arc<PieSlice>()
      .innerRadius(innerRadius)
      .outerRadius(outerRadius);

    const arcsGroup = g
      .append('g')
      .attr('transform', `translate(${centerX},${centerY})`);

    return arcsGroup
      .selectAll<SVGPathElement, PieSlice>('.arc')
      .data(pie(data))
      .join('path')
      .attr('class', 'arc selectable')
      .attr('d', arc)
      .attr('fill', (d) => colorScale(String(d.data[spec.category])))
      .attr('stroke', 'white')
      .attr('stroke-width', 2)
      .style('cursor', 'pointer')
      .style('opacity', 1.0);
  }

  private setupSliceInteraction(
    svg: SVGElement,
    slices: SliceSelection,
    spec: DonutSpec,
  ) {
    slices.on('click', (event: MouseEvent, d: PieSlice) => {
      event.stopPropagation();
      this.handleSliceClick(event, d, slices, spec);
    });

    d3.select(svg).on('click', (event: MouseEvent) => {
      if (event.target === svg) {
        this.clearSelection(slices, spec);
      }
    });
  }

  private handleSliceClick(
    event: MouseEvent,
    d: PieSlice,
    slices: SliceSelection,
    spec: DonutSpec,
  ) {
    const categoryValue = String(d.data[spec.category]);

    if (event.shiftKey) {
      this.toggleSlice(categoryValue);
    } else {
      this.selectSlice(categoryValue);
    }

    this.updateSliceOpacity(slices, spec.category);
    this.applySelectionFilter(spec.category);
  }

  private toggleSlice(category: string) {
    if (this.selectedSlices.has(category)) {
      this.selectedSlices.delete(category);
    } else {
      this.selectedSlices.add(category);
    }
  }

  private selectSlice(category: string) {
    this.selectedSlices.clear();
    this.selectedSlices.add(category);
  }

  private clearSelection(slices: SliceSelection, spec: DonutSpec) {
    this.selectedSlices.clear();
    this.updateSliceOpacity(slices, spec.category);
    this.onFilterRequest?.([]);
  }

  private applySelectionFilter(categoryColumn: string) {
    if (this.selectedSlices.size > 0) {
      this.onFilterRequest?.([
        {col: categoryColumn, op: FilterOp.In, val: [...this.selectedSlices]},
      ]);
    } else {
      this.onFilterRequest?.([]);
    }
  }

  private updateSliceOpacity(slices: SliceSelection, categoryColumn: string) {
    if (this.selectedSlices.size === 0) {
      slices.style('opacity', 1.0);
    } else {
      slices.style('opacity', (d) =>
        this.selectedSlices.has(String(d.data[categoryColumn])) ? 1.0 : 0.2,
      );
    }
  }

  private setupSliceTooltips(slices: SliceSelection, spec: DonutSpec) {
    this.setupTooltip(slices, (d) => {
      const percentage = ((d.endAngle - d.startAngle) / (2 * Math.PI)) * 100;
      return TooltipFormatter.formatFields([
        [spec.category, d.data[spec.category]],
        [spec.value, d.data[spec.value]],
        ['Percentage', `${percentage.toFixed(1)}%`],
      ]);
    });
  }

  private drawLegend(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    colorScale: d3.ScaleOrdinal<string, string>,
    chartWidth: number,
    slices: SliceSelection,
    spec: DonutSpec,
  ) {
    const legend = g
      .append('g')
      .attr('class', 'legend')
      .attr('transform', `translate(${chartWidth + 20}, 0)`);

    const items = legend
      .selectAll<SVGGElement, string>('.legend-item')
      .data(colorScale.domain())
      .join('g')
      .attr('class', 'legend-item')
      .attr('transform', (_, i) => `translate(0, ${i * 20})`)
      .style('cursor', 'pointer')
      .on('click', (_, category) => {
        this.toggleSlice(category);
        this.updateSliceOpacity(slices, spec.category);
        this.applySelectionFilter(spec.category);
      });

    items
      .append('rect')
      .attr('width', 15)
      .attr('height', 15)
      .attr('fill', (d) => colorScale(d));

    items
      .append('text')
      .attr('x', 20)
      .attr('y', 12)
      .style('font-size', '12px')
      .text((d) => d);
  }
}
