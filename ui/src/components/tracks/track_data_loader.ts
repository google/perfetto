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
    yield: () => Promise<undefined>;
  };
};

/**
 * Result of a TrackDataLoader.onUpdate() call.
 *
 * - 'updated': New data was fetched and/or render state was recomputed. The
 *   caller should read getActiveBuffer() and getRenderGlobalState() and update
 *   its local state.
 * - 'aborted': The operation was aborted because the viewport changed during
 *   processing. The caller should not update its state; a new onUpdate() call
 *   will be triggered automatically.
 * - 'unchanged': The cached data is still valid and no render state recompute
 *   was needed. The caller can continue using its existing state.
 */
export type UpdateResult = 'updated' | 'aborted' | 'unchanged';

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
 *   long-running operations (row conversion, render state computation) to keep
 *   the UI responsive. Uses the Scheduler.yield() API when available, falling
 *   back to setTimeout.
 *
 * - **Abort detection**: Monitors for viewport changes during async operations.
 *   If the viewport changes mid-processing, the operation is aborted early to
 *   avoid wasted work, and a canvas redraw is scheduled to trigger a fresh
 *   update.
 *
 * - **Render state computation**: Computes aggregate state (e.g., max depth)
 *   from the loaded data, which can be used by the track for layout decisions.
 *
 * ## Usage
 *
 * ```typescript
 * // In your track's constructor:
 * this.loader = new TrackDataLoader(
 *   trace,
 *   (rawSql, key) => `SELECT ... FROM mipmap(${key.start}, ${key.end}, ...)`,
 *   (row) => this.rowToSlice(row),
 *   undefined,  // or a Monitor for render state invalidation
 *   () => ({maxDepth: 0}),
 *   (slice, state) => { state.maxDepth = Math.max(state.maxDepth, slice.depth); },
 * );
 *
 * // In your track's onUpdate():
 * const result = await this.loader.onUpdate(sql, rowSpec, ctx);
 * if (result === 'updated') {
 *   this.data = this.loader.getActiveBuffer();
 *   this.cacheKey = this.loader.getCacheKey();
 *   this.maxDepth = this.loader.getRenderGlobalState()?.maxDepth ?? 0;
 * }
 * ```
 *
 * @template RawRow The row type returned by the SQL query (must extend Row).
 * @template ResultRow The converted row type stored in the buffer.
 * @template RenderGlobalState Aggregate state computed from all rows.
 */
export class TrackDataLoader<RawRow extends Row, ResultRow, RenderGlobalState> {
  private lastRawSql?: string;
  private key = CacheKey.zero();
  private lastYield: number = 0;
  private readonly queryMonitor = new Monitor([() => this.lastRawSql]);

  private buffers: [Array<ResultRow>, Array<ResultRow>] = [[], []];
  private activeBufferIdx: 0 | 1 = 0;

  private latestRenderState?: RenderGlobalState;

  private readonly yield: () => Promise<undefined>;

  /**
   * Creates a new TrackDataLoader.
   *
   * @param trace The trace object for engine access and RAF scheduling.
   * @param sqlProvider A function that generates the SQL query given the raw
   *   SQL source and a normalized cache key. The cache key provides start, end,
   *   and bucketSize for viewport-based queries.
   * @param converter A function that converts a raw SQL row to the result type.
   *   Called once per row during data loading.
   * @param renderMonitor Optional monitor that triggers render state
   *   recomputation when its state changes (e.g., when hover state changes).
   * @param renderGlobalStateFactory Factory function that creates a fresh
   *   render state object for accumulation.
   * @param updateRenderStateForRow Called for each row to update the aggregate
   *   render state (e.g., tracking max depth).
   */
  constructor(
    private readonly trace: Trace,
    private readonly sqlProvider: (rawSql: string, key: CacheKey) => string,
    private readonly converter: (raw: RawRow) => ResultRow,
    private readonly renderMonitor: Monitor | undefined,
    private readonly renderGlobalStateFactory: () => RenderGlobalState,
    private readonly updateRenderStateForRow: (
      result: ResultRow,
      state: RenderGlobalState,
    ) => void,
  ) {
    const setTimeoutYield = () =>
      new Promise<undefined>((resolve) => setTimeout(resolve, 0));
    this.yield =
      window.scheduler?.yield.bind(window.scheduler) ?? setTimeoutYield;
  }

  /**
   * Main update method called from a track's onUpdate().
   *
   * This method:
   * 1. Checks if new data needs to be fetched (SQL changed or viewport moved)
   * 2. If needed, executes the SQL query and converts rows to ResultRow
   * 3. If render state needs recomputing, iterates through data to build state
   * 4. Throughout, periodically yields and checks for viewport changes
   *
   * @param rawSql The raw SQL source (e.g., from getSqlSource()).
   * @param spec The row specification for iterating query results.
   * @param ctx The track update context with viewport info and abort detection.
   * @returns 'updated' if new data/state is available, 'aborted' if viewport
   *   changed, 'unchanged' if cached data is still valid.
   */
  async onUpdate(
    rawSql: string,
    spec: RawRow,
    ctx: TrackUpdateContext,
  ): Promise<UpdateResult> {
    const rawKey = createCacheKeyFromCtx(ctx);

    this.lastRawSql = rawSql;
    this.lastYield = performance.now();

    let buffer: Array<ResultRow> | undefined;
    if (this.queryMonitor.ifStateChanged() || !rawKey.isCoveredBy(this.key)) {
      const key = rawKey.normalize();
      const result = await this.trace.engine.query(
        this.sqlProvider(rawSql, key),
      );
      // We just came from a yield, so update lastYield to avoid immediate
      // subsequent yields.
      this.lastYield = performance.now();

      let i = 0;
      buffer = this.otherBuffer;
      for (const it = result.iter(spec); it.valid(); it.next(), ++i) {
        if (await this.maybeYieldAndCheckAbort(key, ctx, i)) {
          return 'aborted';
        }
        const res = this.converter(it);
        if (i < buffer.length) {
          buffer[i] = res;
        } else {
          buffer.push(res);
        }
      }
      buffer.length = i;
      this.key = key;
    }
    if (this.renderMonitor?.ifStateChanged() || buffer !== undefined) {
      if (buffer === undefined) {
        const active = this.activeBuffer;
        buffer = this.otherBuffer;
        buffer.length = active.length;
        for (let i = 0; i < active.length; i++) {
          if (await this.maybeYieldAndCheckAbort(this.key, ctx, i)) {
            return 'aborted';
          }
          buffer[i] = {...active[i]};
        }
      }
      const renderGlobalState = this.renderGlobalStateFactory();
      for (let i = 0; i < buffer.length; i++) {
        if (await this.maybeYieldAndCheckAbort(this.key, ctx, i)) {
          return 'aborted';
        }
        this.updateRenderStateForRow(buffer[i], renderGlobalState);
      }
      this.latestRenderState = renderGlobalState;
      this.activeBufferIdx = this.activeBufferIdx === 0 ? 1 : 0;
      this.trace.raf.scheduleCanvasRedraw();
      return 'updated';
    }
    return 'unchanged';
  }

  /**
   * Returns the currently active data buffer.
   *
   * This buffer contains the most recently completed data load. It remains
   * stable while a new load is in progress (written to the other buffer).
   */
  getActiveBuffer(): Array<ResultRow> {
    return this.activeBuffer;
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
   * Returns the most recently computed render global state.
   *
   * Returns undefined if no data has been loaded yet.
   */
  getRenderGlobalState(): RenderGlobalState | undefined {
    return this.latestRenderState;
  }

  private get activeBuffer(): Array<ResultRow> {
    return this.buffers[this.activeBufferIdx];
  }

  private get otherBuffer(): Array<ResultRow> {
    return this.buffers[1 - this.activeBufferIdx];
  }

  private async maybeYieldAndCheckAbort(
    originalKey: CacheKey,
    ctx: TrackUpdateContext,
    i: number,
  ): Promise<boolean> {
    // Check for abort every 1000 iterations to avoid excessive overhead.
    if (i % 1000 !== 0) {
      return false;
    }
    // Yield if more than 4ms have passed since the last yield.
    const TIME_BETWEEN_YIELDS_MS = 4;
    if (performance.now() - this.lastYield > TIME_BETWEEN_YIELDS_MS) {
      await this.yield();
      this.lastYield = performance.now();
    }
    return this.checkAbort(originalKey, ctx);
  }

  private checkAbort(originalKey: CacheKey, ctx: TrackUpdateContext): boolean {
    const latestCtx = ctx.latestDisplayContext();
    const latestKey = createCacheKeyFromCtx(latestCtx);
    if (!latestKey.normalize().equals(originalKey)) {
      this.trace.raf.scheduleCanvasRedraw();
      return true;
    }
    return false;
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
