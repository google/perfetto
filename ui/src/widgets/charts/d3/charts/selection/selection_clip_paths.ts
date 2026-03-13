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
 * Manages SVG clip paths for visual highlighting during brush selections.
 *
 * This utility creates and manages clip paths that can be used to mask/highlight
 * selected regions in charts. It's particularly useful for line-based charts
 * (CDF, line charts) and future charts like box plots and violin plots.
 *
 * Usage:
 * ```typescript
 * const clipPaths = new SelectionClipPaths(svg);
 * const clipUrl = clipPaths.createRectClip(x, y, width, height);
 * group.attr('clip-path', clipUrl);
 * // Later, when clearing selection:
 * clipPaths.removeAllClips();
 * ```
 */
export class SelectionClipPaths {
  private svg: d3.Selection<SVGSVGElement, unknown, null, undefined>;
  private defs: d3.Selection<SVGDefsElement, unknown, null, undefined>;
  private clipPathCounter: number = 0;
  private activeClips: Set<string> = new Set();

  constructor(svg: d3.Selection<SVGSVGElement, unknown, null, undefined>) {
    this.svg = svg;
    this.defs = this.ensureDefs();
  }

  /**
   * Ensures a <defs> element exists in the SVG, creating it if necessary.
   */
  private ensureDefs(): d3.Selection<SVGDefsElement, unknown, null, undefined> {
    let defs = this.svg.select<SVGDefsElement>('defs');
    if (defs.empty()) {
      defs = this.svg.append('defs');
    }
    return defs;
  }

  /**
   * Creates a rectangular clip path and returns its URL reference.
   *
   * @param x - X coordinate of the clip rectangle
   * @param y - Y coordinate of the clip rectangle
   * @param width - Width of the clip rectangle
   * @param height - Height of the clip rectangle
   * @returns CSS url() reference to the clip path (e.g., "url(#clip-0)")
   */
  createRectClip(x: number, y: number, width: number, height: number): string {
    const id = `clip-${this.clipPathCounter++}`;
    this.activeClips.add(id);

    this.defs
      .append('clipPath')
      .attr('id', id)
      .append('rect')
      .attr('x', x)
      .attr('y', y)
      .attr('width', width)
      .attr('height', height);

    return `url(#${id})`;
  }

  /**
   * Removes all active clip paths from the SVG.
   * Call this when clearing brush selections.
   */
  removeAllClips(): void {
    this.activeClips.forEach((id) => {
      this.defs.select(`#${id}`).remove();
    });
    this.activeClips.clear();
  }

  /**
   * Removes a specific clip path by its ID.
   *
   * @param clipUrl - The clip path URL (e.g., "url(#clip-0)")
   */
  removeClip(clipUrl: string): void {
    // Extract ID from url(#clip-0) format
    const match = clipUrl.match(/url\(#(.+)\)/);
    if (match) {
      const id = match[1];
      this.defs.select(`#${id}`).remove();
      this.activeClips.delete(id);
    }
  }
}
