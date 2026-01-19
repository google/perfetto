// Copyright (C) 2023 The Android Open Source Project
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

import {Monitor} from '../../base/monitor';
import {Trace} from '../../public/trace';
import {TrackDisplayContext, TrackUpdateContext} from '../../public/track';
import {Row} from '../../trace_processor/query_result';
import {CacheKey} from './timeline_cache';

declare const window: Window & {
  scheduler?: {
    postTask: <T>(
      callback: () => T,
      options?: {priority?: 'user-blocking' | 'user-visible' | 'background'},
    ) => Promise<T>;
  };
};

/**
 * Result of a TrackRenderPipeline.onUpdate() call.
 *
 * - 'updated': New data was fetched. The caller should read getActiveBuffer()
 *   and getGlobalState() to update its local state.
 * - 'aborted': The operation was aborted because the viewport changed during
 *   processing. The caller should not update its state; a new onUpdate() call
 *   will be triggered automatically.
 * - 'unchanged': The cached data is still valid. The caller can continue using
 *   its existing state.
 */
export type UpdateResult = 'updated' | 'aborted' | 'unchanged';

// Check for yield/abort every N iterations.
const CHECK_INTERVAL = 1000;

// Time between yields in ms. Use shorter interval when scheduler API is
// available since it'll yield back to use more immediately if there's no
// other high-priority work.
const YIELD_INTERVAL_MS = window.scheduler !== undefined ? 5 : 10;

/**
 * A helper class for loading and managing track data with support for:
 *
 * - **Viewport-based caching**: Only fetches data when the visible window
 *   extends beyond the cached range, using normalized cache keys for efficient
 *   over-fetching.
 *
 * - **Double buffering**: Maintains two data buffers and swaps between them,
 *   ensuring the render path always has consistent data to display while new
 *   data is being loaded.
 *
 * - **Cooperative multitasking**: Periodically yields to the main thread during
 *   long-running operations to keep the UI responsive.
 *
 * - **Abort detection**: Monitors for viewport changes during async operations.
 *   If the viewport changes mid-processing, the operation is aborted early to
 *   avoid wasted work, and a canvas redraw is scheduled to trigger a fresh
 *   update.
 *
 * - **Global state**: Accumulates state across all rows (e.g., max depth) that
 *   can be used for layout decisions.
 *
 * ## Usage
 *
 * ```typescript
 * // In your track's constructor:
 * this.pipeline = new TrackRenderPipeline(
 *   trace,
 *   (rawSql, key) => `SELECT ... FROM mipmap(${key.start}, ${key.end}, ...)`,
 *   () => ({maxDepth: 0}),
 *   (row, state) => {
 *     const slice = this.rowToSlice(row);
 *     state.maxDepth = Math.max(state.maxDepth, slice.depth);
 *     return slice;
 *   },
 * );
 *
 * // In your track's onUpdate():
 * const result = await this.pipeline.onUpdate(sql, rowSpec, ctx);
 * if (result === 'updated') {
 *   this.data = this.pipeline.getActiveBuffer();
 *   this.cacheKey = this.pipeline.getCacheKey();
 *   this.maxDepth = this.pipeline.getGlobalState()?.maxDepth ?? 0;
 * }
 * ```
 *
 * @template RawRow The row type returned by the SQL query (must extend Row).
 * @template ResultRow The converted row type stored in the buffer.
 * @template GlobalState Aggregate state computed from all rows.
 */
export class TrackRenderPipeline<RawRow extends Row, ResultRow, GlobalState> {
  private lastRawSql?: string;
  private key = CacheKey.zero();
  private readonly queryMonitor = new Monitor([() => this.lastRawSql]);

  private buffers: [Array<ResultRow>, Array<ResultRow>] = [[], []];
  private activeBufferIdx: 0 | 1 = 0;

  private globalState?: GlobalState;

  // Alternate between user-visible and background priorities when yielding.
  // This balances responsiveness with allowing other background tasks to
  // make progress.
  private useUserVisibleYield = false;

  /**
   * Creates a new TrackRenderPipeline.
   *
   * @param trace The trace object for engine access and RAF scheduling.
   * @param sqlProvider A function that generates the SQL query given the raw
   *   SQL source and a normalized cache key. The cache key provides start, end,
   *   and bucketSize for viewport-based queries.
   * @param createState Factory function that creates a fresh global state
   *   object for accumulation.
   * @param onRow A function that converts a raw SQL row to the result type
   *   and updates the global state. Called once per row during data loading.
   */
  constructor(
    private readonly trace: Trace,
    private readonly sqlProvider: (rawSql: string, key: CacheKey) => string,
    private readonly createState: () => GlobalState,
    private readonly onRow: (raw: RawRow, state: GlobalState) => ResultRow,
  ) {}

  /**
   * Main update method called from a track's onUpdate().
   *
   * This method:
   * 1. Checks if new data needs to be fetched (SQL changed or viewport moved)
   * 2. If needed, executes the SQL query and processes rows via onRow callback
   * 3. Throughout, periodically yields and checks for viewport changes
   *
   * @param rawSql The raw SQL source (e.g., from getSqlSource()).
   * @param spec The row specification for iterating query results.
   * @param ctx The track update context with viewport info and abort detection.
   * @returns 'updated' if new data is available, 'aborted' if viewport changed,
   *   'unchanged' if cached data is still valid.
   */
  async onUpdate(
    rawSql: string,
    spec: RawRow,
    ctx: TrackUpdateContext,
  ): Promise<UpdateResult> {
    const rawKey = createCacheKeyFromCtx(ctx);
    this.lastRawSql = rawSql;

    // Check if we can reuse the existing cached data.
    if (!this.queryMonitor.ifStateChanged() && rawKey.isCoveredBy(this.key)) {
      return 'unchanged';
    }

    const key = rawKey.normalize();
    const result = this.trace.engine.queryStreaming(
      this.sqlProvider(rawSql, key),
    );

    this.otherBuffer.length = 0;

    const state = this.createState();
    let lastYield = performance.now();
    for (const it = result.iter(spec); ; ) {
      for (; it.valid(); it.next()) {
        this.otherBuffer.push(this.onRow(it, state));
        if (this.otherBuffer.length % CHECK_INTERVAL !== 0) {
          continue;
        }

        // Check if the viewport has changed; if so, abort and schedule a
        // redraw to trigger a fresh update.
        const latestKey = createCacheKeyFromCtx(ctx.latestDisplayContext());
        if (!latestKey.normalize().equals(key)) {
          this.trace.raf.scheduleCanvasRedraw();
          return 'aborted';
        }

        // Yield to the main thread periodically to keep the UI responsive.
        if (performance.now() - lastYield > YIELD_INTERVAL_MS) {
          await this.yield();
          lastYield = performance.now();
        }
      }
      if (result.isComplete()) {
        break;
      }
      await result.waitMoreRows();
      it.next(); // Advance to newly arrived data.
    }
    this.key = key;
    this.globalState = state;
    this.activeBufferIdx = this.activeBufferIdx === 0 ? 1 : 0;
    this.trace.raf.scheduleCanvasRedraw();
    return 'updated';
  }

  /**
   * Returns the currently active data buffer.
   *
   * This buffer contains the most recently completed data load. It remains
   * stable while a new load is in progress (written to the other buffer).
   */
  getActiveBuffer(): Array<ResultRow> {
    return this.buffers[this.activeBufferIdx];
  }

  /**
   * Returns the cache key for the currently loaded data.
   *
   * Use this for checkerboard rendering to show loading indicators for
   * regions outside the cached range.
   */
  getCacheKey(): CacheKey {
    return this.key;
  }

  /**
   * Returns the global state accumulated from all rows.
   *
   * Returns undefined if no data has been loaded yet.
   */
  getGlobalState(): GlobalState | undefined {
    return this.globalState;
  }

  private get otherBuffer(): Array<ResultRow> {
    return this.buffers[1 - this.activeBufferIdx];
  }

  private yield(): Promise<void> {
    if (window.scheduler !== undefined) {
      const priority = this.useUserVisibleYield ? 'user-visible' : 'background';
      this.useUserVisibleYield = !this.useUserVisibleYield;
      return window.scheduler.postTask(() => {}, {priority});
    }
    return new Promise((resolve) => setTimeout(resolve, 0));
  }
}

function createCacheKeyFromCtx({
  visibleWindow,
  size,
}: TrackDisplayContext): CacheKey {
  const windowSizePx = Math.max(1, size.width);
  const timespan = visibleWindow.toTimeSpan();
  return CacheKey.create(timespan.start, timespan.end, windowSizePx);
}
