// Copyright (C) 2025 The Android Open Source Project
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

import {TimeScale} from '../../base/time_scale';
import {CacheKey} from './timeline_cache';

/**
 * Rectangle specification for offscreen rendering.
 * Times are in the trace's time domain (bigint nanoseconds).
 * Y and height are in pixels.
 */
export interface OffscreenRect {
  startTime: bigint;
  endTime: bigint;
  y: number;
  h: number;
}

/**
 * Configuration for the OffscreenRenderer.
 */
export interface OffscreenRendererConfig {
  /**
   * Undersample factor for offscreen canvas rendering.
   * Values < 1 mean fewer pixels per bucket, so we scale UP during blit.
   * This makes the sampling decision once during offscreen render, then just
   * duplicates pixels during blit - reducing shimmer from re-sampling.
   * Default: 0.5
   */
  oversampleFactor?: number;
}

/**
 * OffscreenRenderer provides efficient pre-rendering of rectangles to an
 * offscreen canvas, which can then be blitted to the main canvas with
 * appropriate scaling and translation.
 *
 * This pattern significantly improves render performance when zoomed out
 * with many rectangles visible by:
 * 1. Batching rectangles by color for efficient rendering
 * 2. Using rect() + fill() instead of individual fillRect() calls
 * 3. Caching the rendered result for reuse during pan/zoom
 * 4. Undersampling to reduce visual shimmer during animation
 *
 * Usage:
 * 1. Call render() when data is updated to pre-render to offscreen canvas
 * 2. Call blit() during render() to draw the cached content to main canvas
 * 3. Call dispose() when the track is destroyed to release memory
 */
export class OffscreenRenderer {
  private canvas?: OffscreenCanvas;
  private ctx?: OffscreenCanvasRenderingContext2D;
  readonly oversampleFactor: number;

  constructor(config: OffscreenRendererConfig = {}) {
    this.oversampleFactor = config.oversampleFactor ?? 0.5;
  }

  /**
   * Get the current offscreen canvas context, if available.
   * This is useful for custom rendering scenarios where the caller
   * needs direct access to the canvas context.
   */
  getContext(): OffscreenCanvasRenderingContext2D | undefined {
    return this.ctx;
  }

  /**
   * Prepare the offscreen canvas for rendering.
   * Creates or resizes the canvas as needed.
   *
   * @param cacheKey The cache key defining the time range and bucket size
   * @param canvasHeight Height of the offscreen canvas in pixels
   * @returns The canvas context, or undefined if creation failed
   */
  prepareCanvas(
    cacheKey: CacheKey,
    canvasHeight: number,
  ): OffscreenCanvasRenderingContext2D | undefined {
    const bucketSize = Number(cacheKey.bucketSize);
    const timeRange = Number(cacheKey.end - cacheKey.start);

    // Canvas width: oversampleFactor pixels per bucket.
    const canvasWidth =
      Math.ceil(timeRange / bucketSize) * this.oversampleFactor;

    // Create or resize offscreen canvas.
    if (
      this.canvas === undefined ||
      this.canvas.width !== canvasWidth ||
      this.canvas.height !== canvasHeight
    ) {
      this.canvas = new OffscreenCanvas(canvasWidth, canvasHeight);
      this.ctx = this.canvas.getContext('2d') ?? undefined;
    }

    if (this.ctx !== undefined) {
      this.ctx.clearRect(0, 0, canvasWidth, canvasHeight);
    }

    return this.ctx;
  }

  /**
   * Convert a time value to offscreen canvas x coordinate.
   */
  timeToX(t: bigint, cacheKey: CacheKey): number {
    const bucketSize = Number(cacheKey.bucketSize);
    return (Number(t - cacheKey.start) / bucketSize) * this.oversampleFactor;
  }

  /**
   * Render entries grouped by color to the offscreen canvas.
   *
   * @param byColor Map of color string to entries with that color
   * @param cacheKey The cache key defining the time range and bucket size
   * @param canvasHeight Height of the offscreen canvas in pixels
   * @param getRect Function to extract rectangle bounds from an entry
   * @param chunkSize Max rects per fill() call to avoid perf cliffs (default 1000)
   */
  render<T>(
    byColor: Map<string, T[]>,
    cacheKey: CacheKey,
    canvasHeight: number,
    getRect: (entry: T) => OffscreenRect,
    chunkSize: number = 1000,
  ): void {
    const ctx = this.prepareCanvas(cacheKey, canvasHeight);
    if (ctx === undefined) return;

    // Draw rectangles batched by color using rect() + fill().
    // Chunk into smaller batches to avoid performance cliffs with large paths.
    for (const [colorString, entries] of byColor) {
      ctx.fillStyle = colorString;
      for (let i = 0; i < entries.length; i += chunkSize) {
        ctx.beginPath();
        const end = Math.min(i + chunkSize, entries.length);
        for (let j = i; j < end; j++) {
          const rect = getRect(entries[j]);
          const x = this.timeToX(rect.startTime, cacheKey);
          const w = Math.max(this.timeToX(rect.endTime, cacheKey) - x, 1);
          ctx.rect(x, rect.y, w, rect.h);
        }
        ctx.fill();
      }
    }
  }

  /**
   * Blit the offscreen canvas to the main canvas with appropriate transform.
   *
   * @param ctx The main canvas rendering context
   * @param timescale The current timescale for coordinate conversion
   * @param cacheKey The cache key used when rendering
   * @returns true if blit was performed, false if no canvas available
   */
  blit(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    cacheKey: CacheKey,
  ): boolean {
    if (this.canvas === undefined) return false;

    const bucketSize = Number(cacheKey.bucketSize);
    const timePerPx = timescale.pxToDuration(1);
    const scaleX = bucketSize / timePerPx / this.oversampleFactor;

    // Round offset to integer pixel to ensure consistent nearest-neighbor
    // sampling during pan. Without this, sub-pixel offsets cause the rounding
    // threshold to cross, making different source pixels get sampled.
    const offsetX = Math.round(timescale.timeToPx(cacheKey.start));

    ctx.save();
    ctx.imageSmoothingQuality = 'high';
    ctx.translate(offsetX, 0);
    ctx.scale(scaleX, 1);
    ctx.drawImage(this.canvas, 0, 0);
    ctx.restore();

    return true;
  }

  /**
   * Check if the offscreen canvas is ready for blitting.
   */
  hasCanvas(): boolean {
    return this.canvas !== undefined;
  }

  /**
   * Release the offscreen canvas memory.
   * Call this when the track is destroyed.
   */
  dispose(): void {
    this.canvas = undefined;
    this.ctx = undefined;
  }
}
