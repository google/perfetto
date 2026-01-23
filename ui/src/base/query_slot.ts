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
 * QuerySlot: Declarative async data fetching for synchronous render cycles.
 *
 * ## Why it exists
 *
 * UI components (Mithril views, canvas tracks) render synchronously but need
 * async data from SQL queries. QuerySlot bridges this gap:
 * - Call `use()` every render cycle with the current parameters
 * - Get back whatever data is available (cached, stale, or undefined)
 * - New queries are scheduled automatically when parameters change
 * - Serial execution prevents race conditions with shared resources (temp tables)
 *
 * ## Usage
 *
 * ```typescript
 * class MyPanel implements m.ClassComponent<Attrs> {
 *   private readonly taskQueue = new SerialTaskQueue();
 *   private readonly dataSlot = new QuerySlot<MyData>(this.taskQueue);
 *
 *   view({attrs}: m.CVnode<Attrs>) {
 *     const result = this.dataSlot.use({
 *       key: {filters: attrs.filters, pagination: this.pagination},
 *       queryFn: () => fetchData(attrs.filters, this.pagination),
 *       retainOn: ['pagination'],  // Show stale data during pagination changes
 *     });
 *
 *     return m('div', result.data ? renderData(result.data) : 'Loading...');
 *   }
 *
 *   onremove() {
 *     // Cancel pending queries and cleanup resources
 *     this.dataSlot.dispose();
 *   }
 * }
 * ```
 *
 * ## Key concepts
 *
 * - **key**: Object identifying the query. Changes trigger re-fetch.
 * - **retainOn**: Key fields that allow showing previous data while fetching.
 *   E.g., `retainOn: ['pagination']` shows old data during scroll for smoothness,
 *   but `filters` changing would show loading state.
 * - **enabled**: Truthy value required before query runs. Use for dependencies
 *   like `enabled: tableResult.data` to wait for a temp table to be created.
 *
 * ## Behavior
 *
 * - Tasks within a queue run serially (no interleaving)
 * - Only the latest pending task per slot is kept (intermediates dropped)
 * - Each slot has a single-entry cache (most recent result)
 */

import {stringifyJsonWithBigints} from './json_utils';

// Simple alias for a function that returns a Promise of type T.
type AsyncFunc<T> = () => Promise<T>;

/**
 * Runs async tasks one at a time with cancellation support.
 *
 * Tasks are keyed by an object reference. If a new task is scheduled with
 * the same key, it replaces any pending task for that key ("latest wins").
 * Tasks that have already started running cannot be cancelled.
 */
export class SerialTaskQueue {
  private pending = new Map<object, AsyncFunc<void>>();
  private running = false;

  /**
   * Schedule a task. If a task with this key is already pending,
   * it gets replaced.
   */
  schedule(key: object, task: AsyncFunc<void>): void {
    this.pending.set(key, task);
    this.runNext();
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
      await task();
    } catch (e) {
      console.error('Task failed:', e);
    } finally {
      this.running = false;
      this.runNext();
    }
  }
}

export interface QueryOptions<T, K extends object> {
  key: K;
  queryFn: AsyncFunc<T>;
  // If provided, query only runs when this is truthy
  // e.g., enabled: viewResult.data
  enabled?: boolean;
  retainOn?: (keyof K)[];
}

export interface QueryResult<T> {
  data: T | undefined;
  isPending: boolean;
  isFresh: boolean;
}

/**
 * A single query slot with a single-entry cache.
 *
 * Created once per query on a component. Multiple slots share a
 * SerialTaskQueue for serialized execution.
 */
export class QuerySlot<T> {
  private cache?: {key: object; keyStr: string; data: T};
  private pendingKey?: object;

  constructor(private readonly queue: SerialTaskQueue) {}

  /**
   * Call every render cycle to get the current query result.
   */
  use<K extends object>(options: QueryOptions<T, K>): QueryResult<T> {
    const {key, queryFn, enabled, retainOn = []} = options;
    const keyStr = stringifyJsonWithBigints(key);

    // Check if we need to schedule a new query
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
      this.pendingKey = key;
      this.queue.schedule(this, async () => {
        const result = await queryFn();
        this.setCache(key, result);
      });
    }

    // Determine what to return
    const isPending = this.pendingKey !== undefined;

    if (!this.cache) {
      return {data: undefined, isPending, isFresh: false};
    }

    // Check if we can use stale data
    const isFresh = this.cache.keyStr === keyStr;
    if (isFresh) {
      return {data: this.cache.data, isPending, isFresh: true};
    }

    // Key differs - can we use stale data?
    const canUseStale = canUseStaleData(
      this.cache.key,
      key,
      retainOn as string[],
    );
    if (canUseStale) {
      return {data: this.cache.data, isPending, isFresh: false};
    }

    // Can't use stale data
    return {data: undefined, isPending, isFresh: false};
  }

  private setCache(key: object, data: T): void {
    const keyStr = stringifyJsonWithBigints(key);
    this.cache = {key, keyStr, data};

    // Clear pending if it matches
    if (
      this.pendingKey &&
      stringifyJsonWithBigints(this.pendingKey) === keyStr
    ) {
      this.pendingKey = undefined;
    }
  }

  /**
   * Dispose this slot. Cancels pending work.
   * Call this in component's onremove() to prevent orphaned queries.
   */
  dispose(): void {
    this.queue.cancel(this);
    this.cache = undefined;
    this.pendingKey = undefined;
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
