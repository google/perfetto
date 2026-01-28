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
import {Row, ChartSpec, Filter} from '../data/types';
import {BrushBehavior, BrushScales} from './brushing';
import {SelectionStrategy, FilterSelectionStrategy} from './selection';
import {Tooltip} from '../tooltip';
import {SelectionClipPaths} from './selection/selection_clip_paths';
import {selectSVG, clearBrush} from '../d3_types';

export interface ChartRenderer {
  onFilterRequest?: (filters: Filter[]) => void;
  render(svg: SVGElement, data: Row[], spec: ChartSpec): void;
  destroy?(svg: SVGElement): void;
}

interface CrosshairElements {
  crosshairGroup: d3.Selection<SVGGElement, unknown, null, undefined>;
  horizontalLinesGroup: d3.Selection<SVGGElement, unknown, null, undefined>;
  dotsGroup: d3.Selection<SVGGElement, unknown, null, undefined>;
}

export abstract class BaseRenderer implements ChartRenderer {
  protected margin = {top: 20, right: 20, bottom: 40, left: 50};
  protected brushBehavior?: BrushBehavior;
  protected selectionStrategy: SelectionStrategy =
    new FilterSelectionStrategy();
  protected clipPaths: SelectionClipPaths | null = null;

  onFilterRequest?: (filters: Filter[]) => void;

  setSelectionStrategy(strategy: SelectionStrategy): void {
    this.selectionStrategy = strategy;
  }

  abstract render(svg: SVGElement, data: Row[], spec: ChartSpec): void;

  protected clear(svg: SVGElement) {
    d3.select(svg).selectAll('*').remove();
    this.clipPaths = new SelectionClipPaths(selectSVG(svg));
  }

  protected getDimensions(svg: SVGElement) {
    const width = Math.max(
      0,
      svg.clientWidth - this.margin.left - this.margin.right,
    );
    const height = Math.max(
      0,
      svg.clientHeight - this.margin.top - this.margin.bottom,
    );
    return {width, height};
  }

  protected createGroup(svg: SVGElement) {
    return d3
      .select(svg)
      .append('g')
      .attr('transform', `translate(${this.margin.left},${this.margin.top})`);
  }

  protected setupTooltip<
    GElement extends d3.BaseType,
    Datum,
    PElement extends d3.BaseType,
    PDatum,
  >(
    selection: d3.Selection<GElement, Datum, PElement, PDatum>,
    contentFn: (d: Datum) => string,
  ) {
    const tooltip = Tooltip.getInstance();

    selection
      .on('mouseover', (event: MouseEvent, d: Datum) => {
        tooltip.show(contentFn(d), event.pageX, event.pageY);
      })
      .on('mousemove', (event: MouseEvent, d: Datum) => {
        tooltip.show(contentFn(d), event.pageX, event.pageY);
      })
      .on('mouseout', () => {
        tooltip.hide();
      });
  }

  protected addGridLines(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    xScale: d3.AxisScale<d3.NumberValue> | d3.AxisScale<string>,
    yScale: d3.AxisScale<d3.NumberValue>,
    width: number,
    height: number,
    showVertical = true,
  ) {
    g.append('g')
      .attr('class', 'grid grid-horizontal')
      .call(
        d3
          .axisLeft(yScale)
          .tickSize(-width)
          .tickFormat(() => ''),
      )
      .call((sel) => sel.select('.domain').remove());

    if (showVertical && 'ticks' in xScale) {
      g.append('g')
        .attr('class', 'grid grid-vertical')
        .attr('transform', `translate(0,${height})`)
        .call(
          d3
            .axisBottom(xScale as d3.AxisScale<d3.NumberValue>)
            .tickSize(-height)
            .tickFormat(() => ''),
        )
        .call((sel) => sel.select('.domain').remove());
    }
  }

  protected drawAxes(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    xScale: d3.AxisScale<d3.NumberValue> | d3.AxisScale<string>,
    yScale: d3.AxisScale<d3.NumberValue> | d3.AxisScale<string>,
    width: number,
    height: number,
    xLabel: string,
    yLabel: string,
    xTickFormat?: (d: d3.NumberValue | string) => string,
    yTickFormat?: (d: d3.NumberValue | string) => string,
  ) {
    const xAxis = d3.axisBottom(
      xScale as d3.AxisScale<d3.NumberValue | string>,
    );
    if (xTickFormat) {
      xAxis.tickFormat(xTickFormat);
    } else if ('ticks' in xScale) {
      (xAxis as d3.Axis<d3.NumberValue>).ticks(10);
    }

    g.append('g')
      .attr('class', 'x-axis')
      .attr('transform', `translate(0,${height})`)
      .call(xAxis)
      .selectAll('text')
      .attr('transform', 'rotate(-45)')
      .style('text-anchor', 'end');

    const yAxis = d3.axisLeft(yScale as d3.AxisScale<d3.NumberValue | string>);
    if (yTickFormat) {
      yAxis.tickFormat(yTickFormat);
    } else {
      yAxis.ticks(5);
    }

    g.append('g').attr('class', 'y-axis').call(yAxis);

    g.append('text')
      .attr('transform', `translate(${width / 2},${height + 35})`)
      .style('text-anchor', 'middle')
      .style('font-size', '12px')
      .text(xLabel);

    g.append('text')
      .attr('transform', 'rotate(-90)')
      .attr('y', -40)
      .attr('x', -height / 2)
      .style('text-anchor', 'middle')
      .style('font-size', '12px')
      .text(yLabel);
  }

  protected setupBrush(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    data: Row[],
    spec: ChartSpec,
    scales: BrushScales,
    width: number,
    height: number,
    getData?: (d: unknown) => Row,
  ) {
    if (!this.brushBehavior) return;

    const brush = this.brushBehavior.createBrush(width, height);

    brush.on('end', (event: d3.D3BrushEvent<unknown>) => {
      if (event.selection === null) {
        this.selectionStrategy.onClear({
          g,
          allData: data,
          onFilterRequest: this.onFilterRequest,
        });
        return;
      }

      const result = this.brushBehavior!.onBrushEnd(
        {
          type: Array.isArray(event.selection[0]) ? '2d' : '1d',
          extent: event.selection,
        },
        data,
        spec,
        scales,
      );

      this.selectionStrategy.onSelection(result.selectedData, result.filters, {
        g,
        allData: data,
        onFilterRequest: this.onFilterRequest,
        getData,
      });
    });

    g.append('g')
      .attr('class', 'brush')
      .call(brush as d3.BrushBehavior<unknown>);
  }

  protected renderEmptyState(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    width: number,
    height: number,
  ) {
    g.append('text')
      .attr('x', width / 2)
      .attr('y', height / 2)
      .style('text-anchor', 'middle')
      .text('No data');
  }

  protected renderLegend(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    categories: string[],
    colorScale: d3.ScaleOrdinal<string, string>,
    width: number,
    onCategoryClick?: (category: string) => void,
  ) {
    const legend = g
      .append('g')
      .attr('class', 'legend')
      .attr('transform', `translate(${width - 100}, 10)`);

    for (let i = 0; i < categories.length; i++) {
      const category = categories[i];
      const legendRow = legend
        .append('g')
        .attr('transform', `translate(0, ${i * 20})`)
        .style('cursor', onCategoryClick ? 'pointer' : 'default');

      if (onCategoryClick) {
        legendRow.on('click', () => onCategoryClick(category));
      }

      legendRow
        .append('rect')
        .attr('width', 10)
        .attr('height', 10)
        .attr('fill', colorScale(category));

      legendRow
        .append('text')
        .attr('x', 15)
        .attr('y', 9)
        .style('font-size', '11px')
        .text(String(category).substring(0, 20));
    }
  }

  protected clearBrushVisuals(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    elementClass: string,
  ) {
    g.selectAll(`.${elementClass}-dimmed`).remove();
    g.selectAll(`.${elementClass}-highlight`).remove();
    this.clipPaths?.removeAllClips();
    g.selectAll(
      `.${elementClass}-group, .${elementClass}-cell, .${elementClass}-line, .${elementClass}-path`,
    ).style('opacity', 1);
  }

  protected setup2DBrush(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    width: number,
    height: number,
    data: Row[],
    onBrushEnd: (selection: [[number, number], [number, number]]) => void,
    onClear: () => void,
  ): d3.BrushBehavior<unknown> {
    const brush = d3
      .brush()
      .extent([
        [0, 0],
        [width, height],
      ])
      .on('end', (event: d3.D3BrushEvent<unknown>) => {
        if (event.selection === null) {
          onClear();
          this.selectionStrategy?.onClear({
            g,
            allData: data,
            onFilterRequest: this.onFilterRequest,
          });
          return;
        }

        onBrushEnd(event.selection as [[number, number], [number, number]]);
      });

    g.append('g').attr('class', 'brush').call(brush);

    g.on('click', (event: MouseEvent) => {
      if (
        event.target === event.currentTarget ||
        d3.select(event.target as Element).classed('overlay')
      ) {
        clearBrush(brush, g);
        onClear();
        this.selectionStrategy?.onClear({
          g,
          allData: data,
          onFilterRequest: this.onFilterRequest,
        });
      }
    });

    return brush;
  }

  protected createCrosshairElements(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    height: number,
  ): CrosshairElements {
    const crosshairGroup = g
      .append('g')
      .attr('class', 'crosshair')
      .style('display', 'none')
      .style('pointer-events', 'none');

    crosshairGroup
      .append('line')
      .attr('class', 'crosshair-vertical')
      .attr('y1', 0)
      .attr('y2', height)
      .attr('stroke', '#666')
      .attr('stroke-width', 1)
      .attr('stroke-dasharray', '4,4');

    const horizontalLinesGroup = crosshairGroup
      .append('g')
      .attr('class', 'crosshair-horizontals');

    const dotsGroup = crosshairGroup
      .append('g')
      .attr('class', 'crosshair-dots');

    return {crosshairGroup, horizontalLinesGroup, dotsGroup};
  }

  destroy(svg: SVGElement) {
    this.clear(svg);
  }
}
