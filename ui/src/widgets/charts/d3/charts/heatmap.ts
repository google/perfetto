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
import {OpacitySelectionStrategy} from './selection/opacity_selection_strategy';
import {TooltipFormatter} from '../tooltip';

type HeatmapSpec = Extract<ChartSpec, {type: ChartType.Heatmap}>;

interface HeatmapCell {
  [key: string]: string | number;
  x: string;
  y: string;
  value: number;
}

interface HeatmapScales {
  x: d3.ScaleBand<string>;
  y: d3.ScaleBand<string>;
  color: d3.ScaleSequential<string>;
}

export class HeatmapRenderer extends BaseRenderer {
  constructor() {
    super();
    this.selectionStrategy = new OpacitySelectionStrategy();
  }

  render(svg: SVGElement, data: Row[], spec: ChartSpec) {
    if (spec.type !== ChartType.Heatmap) return;

    this.clear(svg);
    const {width, height} = this.getDimensions(svg);
    const g = this.createGroup(svg);

    // Data is already aggregated by the data source
    const cells = this.toCells(data, spec);

    if (cells.length === 0) {
      this.renderEmptyState(g, width, height);
      return;
    }

    const scales = this.createScales(cells, width, height);

    this.setupHeatmapBrush(g, cells, data, spec, scales, width, height);
    this.drawCells(g, cells, data, spec, scales);
    this.drawAxes(g, scales.x, scales.y, width, height, spec.x, spec.y);
    this.renderColorLegend(g, scales.color, width, spec.value);
  }

  private toCells(data: Row[], spec: HeatmapSpec): HeatmapCell[] {
    // Data source has already aggregated, just convert to typed cells
    return data
      .map((row) => ({
        x: String(row[spec.x]),
        y: String(row[spec.y]),
        value: Number(row[spec.value]) || 0,
      }))
      .filter((cell) => cell.x !== 'undefined' && cell.y !== 'undefined');
  }

  private createScales(
    cells: HeatmapCell[],
    width: number,
    height: number,
  ): HeatmapScales {
    const xValues = [...new Set(cells.map((d) => d.x))].sort();
    const yValues = [...new Set(cells.map((d) => d.y))].sort();

    return {
      x: d3.scaleBand().domain(xValues).range([0, width]).padding(0.05),
      y: d3.scaleBand().domain(yValues).range([0, height]).padding(0.05),
      color: d3
        .scaleSequential(d3.interpolateBlues)
        .domain([0, d3.max(cells, (d) => d.value) ?? 100]),
    };
  }

  private drawCells(
    container: d3.Selection<SVGGElement, unknown, null, undefined>,
    cells: HeatmapCell[],
    data: Row[],
    spec: HeatmapSpec,
    scales: HeatmapScales,
    opacity = 1.0,
    // When rendering clipped brush overlays, we create multiple stacked layers of cells.
    // Tooltips must be disabled on dimmed/highlight layers to prevent duplicate tooltips.
    enableTooltip = true,
  ) {
    const rects = container
      .selectAll<SVGRectElement, HeatmapCell>('.heatmap-cell')
      .data(cells)
      .enter()
      .append('rect')
      .attr('class', 'heatmap-cell selectable')
      .attr('x', (d) => scales.x(d.x) ?? 0)
      .attr('y', (d) => scales.y(d.y) ?? 0)
      .attr('width', scales.x.bandwidth())
      .attr('height', scales.y.bandwidth())
      .attr('fill', (d) => scales.color(d.value))
      .attr('stroke', '#fff')
      .attr('stroke-width', 1)
      .style('opacity', opacity)
      .style('cursor', 'pointer')
      .style('pointer-events', 'all')
      .on('click', (_event, d) => {
        this.selectionStrategy.onSelection(
          [d],
          [
            {col: spec.x, op: FilterOp.Eq, val: d.x},
            {col: spec.y, op: FilterOp.Eq, val: d.y},
          ],
          {
            g: container,
            allData: data,
            onFilterRequest: this.onFilterRequest,
            updateSourceFilter: true,
          },
        );
      });

    if (enableTooltip) {
      this.setupTooltip(rects, (d: HeatmapCell) =>
        TooltipFormatter.formatFields([
          [spec.x, d.x],
          [spec.y, d.y],
          [spec.value, d.value],
        ]),
      );
    }
  }

  private setupHeatmapBrush(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    cells: HeatmapCell[],
    data: Row[],
    spec: HeatmapSpec,
    scales: HeatmapScales,
    width: number,
    height: number,
  ) {
    this.setup2DBrush(
      g,
      width,
      height,
      data,
      (selection) => {
        this.handleBrushSelection(g, selection, cells, data, spec, scales);
      },
      () => {
        this.clearBrushVisuals(g, 'heatmap');
        this.selectionStrategy?.onClear({
          g,
          allData: data,
          onFilterRequest: this.onFilterRequest,
          updateSourceFilter: true,
        });
      },
    );
  }

  private handleBrushSelection(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    selection: [[number, number], [number, number]],
    cells: HeatmapCell[],
    data: Row[],
    spec: HeatmapSpec,
    scales: HeatmapScales,
  ) {
    const [[x0, y0], [x1, y1]] = selection;

    const selectedX = this.getSelectedBandValues(scales.x, x0, x1);
    const selectedY = this.getSelectedBandValues(scales.y, y0, y1);

    this.clearBrushVisuals(g, 'heatmap');

    if (this.selectionStrategy?.usesClipPaths()) {
      this.renderClippedCells(g, cells, data, spec, scales, x0, y0, x1, y1);
    } else {
      g.selectAll('.heatmap-cell').style('opacity', 0.7);
    }

    const filters = this.createFilters(
      spec,
      selectedX,
      selectedY,
      scales.x.domain().length,
      scales.y.domain().length,
    );

    const selectedCells = cells.filter(
      (d) => selectedX.includes(d.x) && selectedY.includes(d.y),
    );

    this.selectionStrategy?.onSelection(selectedCells, filters, {
      g,
      allData: data,
      onFilterRequest: this.onFilterRequest,
      updateSourceFilter: true,
    });
  }

  private getSelectedBandValues(
    scale: d3.ScaleBand<string>,
    min: number,
    max: number,
  ): string[] {
    const bandwidth = scale.bandwidth();

    return scale.domain().filter((val) => {
      const pos = scale(val);
      return pos !== undefined && min < pos + bandwidth && max > pos;
    });
  }

  private renderClippedCells(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    cells: HeatmapCell[],
    data: Row[],
    spec: HeatmapSpec,
    scales: HeatmapScales,
    x0: number,
    y0: number,
    x1: number,
    y1: number,
  ) {
    g.selectAll('.heatmap-cell').style('opacity', 0);

    // Dimmed layer
    this.drawCells(
      g.append('g').attr('class', 'heatmap-dimmed'),
      cells,
      data,
      spec,
      scales,
      0.2,
      false,
    );

    // Highlighted layer with clip
    if (this.clipPaths) {
      const clipUrl = this.clipPaths.createRectClip(x0, y0, x1 - x0, y1 - y0);

      this.drawCells(
        g
          .append('g')
          .attr('class', 'heatmap-highlight')
          .attr('clip-path', clipUrl),
        cells,
        data,
        spec,
        scales,
        1.0,
        false,
      );
    }
  }

  private createFilters(
    spec: HeatmapSpec,
    selectedX: string[],
    selectedY: string[],
    totalX: number,
    totalY: number,
  ): Filter[] {
    const filters: Filter[] = [];

    if (selectedX.length > 0 && selectedX.length < totalX) {
      filters.push({col: spec.x, op: FilterOp.In, val: selectedX});
    }
    if (selectedY.length > 0 && selectedY.length < totalY) {
      filters.push({col: spec.y, op: FilterOp.In, val: selectedY});
    }

    return filters;
  }

  private renderColorLegend(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    colorScale: d3.ScaleSequential<string>,
    width: number,
    valueLabel: string,
  ) {
    const legendWidth = 20;
    const legendHeight = 200;
    const legendX = width + 40;
    const legendY = 20;

    const gradientId = 'heatmap-gradient';
    this.createGradient(g, gradientId, colorScale);

    // Legend rect
    g.append('rect')
      .attr('x', legendX)
      .attr('y', legendY)
      .attr('width', legendWidth)
      .attr('height', legendHeight)
      .style('fill', `url(#${gradientId})`)
      .attr('stroke', '#ccc');

    // Legend axis
    const legendScale = d3
      .scaleLinear()
      .domain(colorScale.domain())
      .range([legendY + legendHeight, legendY]);

    g.append('g')
      .attr('transform', `translate(${legendX + legendWidth}, 0)`)
      .call(d3.axisRight(legendScale).ticks(5).tickFormat(d3.format('.2s')));

    // Legend label
    g.append('text')
      .attr('x', legendX + legendWidth / 2)
      .attr('y', legendY - 5)
      .style('text-anchor', 'middle')
      .style('font-size', '10px')
      .text(valueLabel);
  }

  private createGradient(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    id: string,
    colorScale: d3.ScaleSequential<string>,
  ) {
    const defs = g.append('defs');
    const gradient = defs
      .append('linearGradient')
      .attr('id', id)
      .attr('x1', '0%')
      .attr('y1', '100%')
      .attr('x2', '0%')
      .attr('y2', '0%');

    const numStops = 10;
    const [min, max] = colorScale.domain();

    for (let i = 0; i <= numStops; i++) {
      const t = i / numStops;
      const value = min + t * (max - min);
      gradient
        .append('stop')
        .attr('offset', `${t * 100}%`)
        .attr('stop-color', colorScale(value));
    }
  }
}
