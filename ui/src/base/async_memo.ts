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

/**
 * AsyncMemo: Declarative async data fetching for synchronous render cycles.
 *
 * ## Why it exists
 *
 * UI components (Mithril views, canvas tracks) render synchronously but need
 * async data e.g. from SQL queries. Manually tracking state changes and
 * handling stale queries is difficult and error prone.
 *
 * AsyncMemo bridges this gap:
 * - Call `use()` every render cycle with the current parameters
 * - Get back whatever data is available (cached, stale, or undefined)
 * - New tasks are scheduled automatically when parameters change
 * - Serial execution prevents race conditions with shared resources (temp
 *   tables)
 *
 * ## Example usage:
 *
 * ```ts
 * class MyPanel implements m.ClassComponent<Attrs> {
 *   private readonly memo = new AsyncMemo<MyData>();
 *
 *   view({attrs}: m.CVnode<Attrs>) {
 *     const result = this.memo.use({
 *       key: {filters: attrs.filters, pagination: this.pagination},
 *       compute: () => fetchData(attrs.filters, this.pagination),
 *       retainOn: ['pagination'],  // Show stale data during pagination changes
 *     });
 *
 *     return m('div', result.data ? renderData(result.data) : 'Loading...');
 *   }
 *
 *   onremove() {
 *     // Cancel pending tasks and cleanup resources
 *     this.memo.dispose();
 *   }
 * }
 * ```
 *
 * ## Key concepts
 *
 * - **key**: Object identifying the memo. Changes trigger re-fetch.
 * - **retainOn**: Key fields that allow showing previous data while fetching.
 *   E.g., `retainOn: ['pagination']` shows old data during scroll for
 *   smoothness, but `filters` changing would show loading state.
 * - **enabled**: Truthy value required before task runs. Use for dependencies
 *   like `enabled: tableResult.data` to wait for a temp table to be created.
 *
 * ## Behavior
 *
 * - Tasks within a queue run serially (no interleaving)
 * - Only the latest pending task per memo is kept (intermediates dropped)
 * - Each memo has a single-entry cache (most recent result)
 * - If compute() returns an AsyncDisposable, it is automatically disposed
 *   before running the next task and when the memo is disposed. Disposal runs
 *   through the queue to stay synchronized with in-flight tasks.
 *
 * ## Error handling
 *
 * If compute() throws, the thrown value is cached for that key in place of
 * data, and use() with the same key returns it as `error` (with
 * `isPending: true` and no data). use() itself never throws a compute error.
 * Like data, a cached error is not recomputed until invalidate() is called or
 * the key changes, and it is never returned for a different key or used as
 * stale data via retainOn.
 *
 * The error is also re-thrown out of the task queue so it surfaces as an
 * unhandled promise rejection, which the app's global error handler (window
 * 'unhandledrejection') reports on screen.
 *
 * Errors from tasks that were superseded (cancelled) before they failed are
 * dropped entirely.
 */

import m from 'mithril';
import {isAsyncDisposable} from './disposable';
import {type JSONCompatible, stringifyJsonWithBigints} from './json_utils';

/**
 * Signal passed to compute() to check if the task has been cancelled.
 * Check this periodically during long-running operations to bail out early.
 */
export interface CancellationSignal {
  readonly isCancelled: boolean;
}

/**
 * Special return value from compute() indicating the task was cancelled.
 * When returned, the result is not cached.
 */
export const TASK_CANCELLED = Symbol('TASK_CANCELLED');

// Simple alias for a function that returns a Promise of type T.
type AsyncFunc<T> = () => Promise<T>;

/**
 * Runs async functions one at a time with cancellation support.
 *
 * Compelling use case - to avoid interlaving queries in async functions that
 * run more than one query.
 *
 * Tasks are keyed by an object reference. If a new task is scheduled with the
 * same key, it replaces any pending task for that key ("latest wins"). Tasks
 * that have already started running cannot be cancelled.
 */
export class AtomicTaskQueue {
  private pending = new Map<object, AsyncFunc<void>>();
  private running = false;

  /**
   * Schedule a task. If a task with this key is already pending,
   * it gets replaced.
   *
   * Returns a promise for the task started by this call, if any. It rejects if
   * that task fails (tasks are not caught here) and resolves immediately if
   * the queue is already busy - the scheduled task then runs later as part of
   * the in-flight drain, and a failure there rejects a promise nobody holds.
   * Either way, ignoring the return value means a failing task surfaces as an
   * unhandled promise rejection reported by the global error handler.
   */
  schedule(key: object, task: AsyncFunc<void>): Promise<void> {
    this.pending.set(key, task);
    return this.runNext();
  }

  /**
   * Cancel any pending task for this key.
   * Has no effect if the task is already running.
   */
  cancel(key: object): void {
    this.pending.delete(key);
  }

  private async runNext(): Promise<void> {
    if (this.running) return;

    const first = this.pending.entries().next();
    if (first.done) return;

    const [key, task] = first.value;
    this.pending.delete(key);

    this.running = true;
    try {
      // Intentionally no catch: a failing task rejects the chain, which
      // callers (or the global unhandledrejection handler) deal with.
      await task();
    } finally {
      this.running = false;
      this.runNext();
    }
  }
}

export interface AsyncMemoOptions<T, K extends JSONCompatible<K>> {
  readonly key: K;
  // Function to call when the key changes to fetch the memo data.
  readonly compute: (
    signal: CancellationSignal,
  ) => Promise<T | typeof TASK_CANCELLED>;
  // If provided, compute() only runs when this is truthy.
  readonly enabled?: boolean;
  // Keep returning the previous memo while the new one loads as long as only
  // keys in this set have changed.
  readonly retainOn?: (keyof K)[];
}

export type AsyncMemoResult<T> =
  | {
      readonly isPending: true;
      readonly data?: T; // Stale data may be available.
      // The value thrown by compute() if it failed for the current key. A
      // failed key stays pending with no data until invalidated or changed.
      readonly error?: unknown;
    }
  | {
      readonly isPending: false;
      readonly data: T;
    };

// A cache entry holds either the data from a successful compute() or the
// error from a failed one.
type Cache<T> = {
  readonly key: object;
  readonly keyStr: string;
} & (
  | {readonly failed: false; readonly data: T}
  | {readonly failed: true; readonly data?: undefined; readonly error: unknown}
);

/**
 * A single async memo with a single-entry cache.
 *
 * Created once per piece of data you want to memoize on a component. Multiple
 * memos should share a SerialTaskQueue for atomic compute task execution.
 *
 * If T is an AsyncDisposable, the memo will automatically dispose of previous
 * results before running a new compute and when the memo is disposed.
 */
export class AsyncMemo<T> {
  private cache?: Cache<T>;
  private pendingKey?: object;
  private disposed = false;
  private currentSignal?: {cancelled: boolean};

  constructor(
    private readonly queue: AtomicTaskQueue = new AtomicTaskQueue(),
  ) {}

  /**
   * Call every render cycle to get the current memoized result.
   *
   * @throws Error if called after dispose()
   */
  use<K extends JSONCompatible<K>>(
    options: AsyncMemoOptions<T, K>,
  ): AsyncMemoResult<T> {
    if (this.disposed) {
      throw new Error('AsyncMemo.use() called after dispose()');
    }
    const {key, compute, enabled, retainOn = []} = options;
    const keyStr = stringifyJsonWithBigints(key);

    // Check if we need to schedule a new task
    const pendingKeyStr = this.pendingKey
      ? stringifyJsonWithBigints(this.pendingKey)
      : undefined;
    const cachedKeyStr = this.cache?.keyStr;

    const isKeyDifferentFromPending = pendingKeyStr !== keyStr;
    const isKeyDifferentFromCache = cachedKeyStr !== keyStr;

    // enabled: undefined means no dependencies (satisfied)
    // enabled: <value> means satisfied only if truthy
    const canRun = enabled === undefined || enabled;

    if (isKeyDifferentFromPending && isKeyDifferentFromCache && canRun) {
      // Cancel any in-flight task
      if (this.currentSignal) {
        this.currentSignal.cancelled = true;
      }

      // Create new signal for this task
      const signal = {cancelled: false};
      this.currentSignal = signal;

      this.pendingKey = key;
      this.queue.schedule(this, async () => {
        try {
          // Dispose of previous result before running new task
          await this.disposeCache();
          const result = await compute({
            get isCancelled() {
              return signal.cancelled;
            },
          });

          this.finaliseTask(key, result);
        } catch (e) {
          // Support both throwing and returning TASK_CANCELLED
          if (e === TASK_CANCELLED) {
            this.finaliseTask(key, TASK_CANCELLED);
          } else if (signal.cancelled) {
            // This task was superseded before it failed. Its result is no
            // longer of interest, so drop the error rather than reporting a
            // stale failure to the global error handler.
          } else {
            // Cache the error in place of data so use() returns it for this
            // key. pendingKey is deliberately left set so the result stays
            // pending (there is no data to return).
            this.cache = {
              key,
              keyStr: stringifyJsonWithBigints(key),
              failed: true,
              error: e,
            };
            // Also let the rejection escape the queue so the app's global
            // unhandledrejection handler reports it.
            throw e;
          }
        } finally {
          m.redraw();
        }
      });
    }

    // Determine what to return
    const isPending = this.pendingKey !== undefined;

    if (!this.cache) {
      // Without cached data, the result remains pending. This also covers a
      // memo blocked by `enabled`, whose dependency has not resolved yet.
      return {data: undefined, isPending: true};
    }

    // Check if we can use cached data
    if (this.cache.keyStr === keyStr) {
      if (this.cache.failed) {
        return {data: undefined, isPending: true, error: this.cache.error};
      }
      return isPending
        ? {data: this.cache.data, isPending: true}
        : {data: this.cache.data, isPending: false};
    }

    // Key differs - can we use stale data? A cached error is never reused for
    // a different key.
    if (this.cache.failed) {
      return {data: undefined, isPending: true};
    }
    const canUseStale = canUseStaleData(
      this.cache.key,
      key,
      retainOn as string[],
    );
    if (canUseStale) {
      return isPending
        ? {data: this.cache.data, isPending: true}
        : {data: this.cache.data, isPending: false};
    }

    // Can't use stale data
    return {data: undefined, isPending: true};
  }

  /**
   * Called when a task completes. Clears the pending state and optionally
   * caches the result (if not cancelled).
   */
  private finaliseTask(key: object, result: T | typeof TASK_CANCELLED): void {
    const keyStr = stringifyJsonWithBigints(key);

    // Clear pending if it matches
    if (
      this.pendingKey &&
      stringifyJsonWithBigints(this.pendingKey) === keyStr
    ) {
      this.pendingKey = undefined;
    }

    // Cache the result (unless cancelled)
    if (result !== TASK_CANCELLED) {
      this.cache = {key, keyStr, failed: false, data: result};
    }
  }

  private async disposeCache(): Promise<void> {
    if (this.cache && isAsyncDisposable(this.cache.data)) {
      await this.cache.data[Symbol.asyncDispose]();
      // Only clear cache for AsyncDisposable - the resource is now invalid
      // For non-AsyncDisposable data, keep the cache until setCache replaces it
      this.cache = undefined;
    }
  }

  /**
   * Clear the cached result (data or error) and any pending state, forcing
   * the next use() with any key to re-run its compute function.
   * Unlike dispose(), the memo remains usable. Cancels any in-flight task and
   * disposes cached AsyncDisposable data through the queue to stay
   * synchronized with pending work.
   */
  invalidate(): void {
    if (this.disposed) return;

    // Cancel any in-flight task and clear pending state so the next use()
    // reschedules from scratch.
    if (this.currentSignal) {
      this.currentSignal.cancelled = true;
      this.currentSignal = undefined;
    }
    this.pendingKey = undefined;

    // Schedule cache disposal/clearing through the queue so it runs after any
    // in-flight task settles.
    this.queue.schedule(this, async () => {
      await this.disposeCache();
      this.cache = undefined;
    });
  }

  /**
   * Dispose this memo. Cancels pending work and disposes any cached
   * AsyncDisposable through the queue to maintain synchronization.
   * Call this in component's onremove() to prevent orphaned tasks.
   */
  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;

    this.queue.cancel(this);
    this.pendingKey = undefined;

    // Schedule cache disposal through the queue. This runs after any
    // in-flight task completes, ensuring we dispose whatever ends up
    // in the cache (either existing data or newly fetched data).
    this.queue.schedule(this, async () => {
      await this.disposeCache();
    });
  }
}

/**
 * Check if stale data can be used by comparing cached and current keys.
 *
 * Returns true if only fields listed in retainOn differ.
 * Returns false if any non-retainOn field differs.
 */
function canUseStaleData(
  cachedKey: object,
  currentKey: object,
  retainOn: string[],
): boolean {
  const cached = cachedKey as Record<string, unknown>;
  const current = currentKey as Record<string, unknown>;

  // Check all fields in current key
  for (const field of Object.keys(current)) {
    const cachedStr = stringifyJsonWithBigints(cached[field]);
    const currentStr = stringifyJsonWithBigints(current[field]);

    if (cachedStr !== currentStr) {
      // Field differs - is it in retainOn?
      if (!retainOn.includes(field)) {
        return false; // Non-retainOn field changed → need fresh data
      }
    }
  }

  // Also check for fields in cached that aren't in current (removed fields)
  for (const field of Object.keys(cached)) {
    if (!(field in current)) {
      if (!retainOn.includes(field)) {
        return false;
      }
    }
  }

  return true; // Only retainOn fields changed → stale OK
}
