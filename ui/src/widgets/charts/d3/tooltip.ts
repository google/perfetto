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

/**
 * Helper class for consistent tooltip formatting across all chart types.
 */
export class TooltipFormatter {
  /**
   * Format a numeric value using SI prefix notation (k, M, G, etc.)
   */
  static formatValue(val: unknown): string {
    const num = Number(val);
    return isNaN(num) ? String(val) : d3.format('.2s')(num);
  }

  /**
   * Format a single field as HTML: <strong>label:</strong> value
   */
  static formatField(label: string, val: unknown): string {
    return `<strong>${label}:</strong> ${this.formatValue(val)}`;
  }

  /**
   * Format multiple fields as HTML, joined with <br/>
   * @param fields Array of [label, value] tuples
   */
  static formatFields(fields: [string, unknown][]): string {
    return fields.map(([k, v]) => this.formatField(k, v)).join('<br/>');
  }

  /**
   * Format a field with custom value formatter
   */
  static formatFieldCustom(
    label: string,
    val: unknown,
    formatter: (v: unknown) => string,
  ): string {
    return `<strong>${label}:</strong> ${formatter(val)}`;
  }
}

export class Tooltip {
  private static instance: Tooltip | undefined;
  private element: d3.Selection<HTMLDivElement, null, d3.BaseType, unknown>;
  private currentContent: string | null = null;

  private constructor() {
    this.element = d3
      .select('body')
      .selectAll<HTMLDivElement, null>('.chart-tooltip')
      .data([null])
      .join('div')
      .attr('class', 'chart-tooltip')
      .style('position', 'absolute')
      .style('visibility', 'hidden')
      .style('pointer-events', 'none')
      .style('z-index', '1000')
      .style('background', 'rgba(0, 0, 0, 0.8)')
      .style('color', 'white')
      .style('padding', '8px')
      .style('border-radius', '4px')
      .style('font-size', '12px')
      .style('box-shadow', '0 2px 4px rgba(0,0,0,0.2)');
  }

  static getInstance(): Tooltip {
    if (!Tooltip.instance) {
      Tooltip.instance = new Tooltip();
    }
    return Tooltip.instance;
  }

  show(content: string, x: number, y: number) {
    if (this.currentContent !== content) {
      this.element.html(content);
      this.currentContent = content;
    }

    this.element
      .style('visibility', 'visible')
      .style('left', `${x + 10}px`)
      .style('top', `${y - 10}px`);
  }

  hide() {
    this.element.style('visibility', 'hidden');
    this.currentContent = null;
  }
}
