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
 *   private readonly executor = new SerialQueryExecutor();
 *   private readonly dataSlot = new QuerySlot<MyData>(this.executor);
 *
 *   view({attrs}: m.CVnode<Attrs>) {
 *     const result = this.dataSlot.use({
 *       key: {filters: attrs.filters, pagination: this.pagination},
 *       queryFn: () => fetchData(attrs.filters, this.pagination),
 *       staleOn: ['pagination'],  // Show stale data during pagination changes
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
 * - **staleOn**: Key fields that allow showing previous data while fetching.
 *   E.g., `staleOn: ['pagination']` shows old data during scroll for smoothness,
 *   but `filters` changing would show loading state.
 * - **dependsOn**: Truthy value required before query runs. Use for dependencies
 *   like `dependsOn: tableResult.data` to wait for a temp table to be created.
 *
 * ## Behavior
 *
 * - Queries within an executor run serially (no interleaving)
 * - Only the latest pending query per slot is kept (intermediates dropped)
 * - Each slot has a single-entry cache (most recent result)
 */

import m from 'mithril';
import {stringifyJsonWithBigints} from './json_utils';

export interface QueryOptions<T, K extends object> {
  key: K;
  queryFn: () => Promise<T>;
  // If provided, query only runs when this is truthy
  // e.g., dependsOn: viewResult.data
  enabled?: unknown;
  retainOn?: (keyof K)[];
}

export interface QueryResult<T> {
  data: T | undefined;
  isPending: boolean;
  isFresh: boolean;
}

interface PendingWork<T> {
  key: object;
  queryFn: () => Promise<T>;
  staleOn: string[];
}

/**
 * Executes queries serially across multiple QuerySlots.
 *
 * - Tracks pending work per slot (each slot keeps only its latest)
 * - Runs one query at a time globally (no interleaving)
 * - Respects dependency order (slot A before slot B if B depends on A)
 */
export class SerialQueryExecutor {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  private pending = new Map<QuerySlot<any>, PendingWork<any>>();
  private running = false;

  /**
   * Schedule work for a slot. Replaces any existing pending work for this slot.
   */
  schedule<T>(slot: QuerySlot<T>, work: PendingWork<T>): void {
    this.pending.set(slot, work);
    this.tryRunNext();
  }

  /**
   * Cancel any pending work for a slot.
   * Called when a slot is disposed to prevent orphaned queries.
   */
  cancel(slot: QuerySlot<unknown>): void {
    this.pending.delete(slot);
  }

  private async tryRunNext(): Promise<void> {
    if (this.running) return;

    const slot = this.pickRunnableSlot();
    if (!slot) return;

    const work = this.pending.get(slot)!;
    this.pending.delete(slot);

    this.running = true;
    try {
      const result = await work.queryFn();
      slot.setCache(work.key, result);
      m.redraw();
    } catch (e) {
      // TODO: handle errors properly
      console.error('Query failed:', e);
    } finally {
      this.running = false;
      this.tryRunNext();
    }
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  private pickRunnableSlot(): QuerySlot<any> | undefined {
    // All pending work has satisfied dependencies (checked at schedule time)
    // Just return the first one
    for (const [slot] of this.pending) {
      return slot;
    }
    return undefined;
  }
}

/**
 * A single query slot with a single-entry cache.
 *
 * Created once per query on a component. Multiple slots share a
 * SerialQueryExecutor for serialized execution.
 */
export class QuerySlot<T> {
  private cache?: {key: object; keyStr: string; data: T};
  private pendingKey?: object;

  constructor(private readonly executor: SerialQueryExecutor) {}

  /**
   * Call every render cycle to get the current query result.
   */
  use<K extends object>(options: QueryOptions<T, K>): QueryResult<T> {
    const {key, queryFn, enabled: dependsOn, retainOn: staleOn = []} = options;
    const keyStr = stringifyJsonWithBigints(key);

    // Check if we need to schedule a new query
    const pendingKeyStr = this.pendingKey
      ? stringifyJsonWithBigints(this.pendingKey)
      : undefined;
    const cachedKeyStr = this.cache?.keyStr;

    const isKeyDifferentFromPending = pendingKeyStr !== keyStr;
    const isKeyDifferentFromCache = cachedKeyStr !== keyStr;

    // dependsOn: undefined means no dependencies (satisfied)
    // dependsOn: <value> means satisfied only if truthy
    const depsSatisfied = dependsOn === undefined || !!dependsOn;

    if (isKeyDifferentFromPending && isKeyDifferentFromCache && depsSatisfied) {
      this.pendingKey = key;
      this.executor.schedule(this, {
        key,
        queryFn,
        staleOn: staleOn as string[],
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
      staleOn as string[],
    );
    if (canUseStale) {
      return {data: this.cache.data, isPending, isFresh: false};
    }

    // Can't use stale data
    return {data: undefined, isPending, isFresh: false};
  }

  /**
   * Called by executor when query completes.
   */
  setCache(key: object, data: T): void {
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
    this.executor.cancel(this);
    this.cache = undefined;
    this.pendingKey = undefined;
  }
}

/**
 * Check if stale data can be used by comparing cached and current keys.
 *
 * Returns true if only fields listed in staleOn differ.
 * Returns false if any non-staleOn field differs.
 */
function canUseStaleData(
  cachedKey: object,
  currentKey: object,
  staleOn: string[],
): boolean {
  const cached = cachedKey as Record<string, unknown>;
  const current = currentKey as Record<string, unknown>;

  // Check all fields in current key
  for (const field of Object.keys(current)) {
    const cachedStr = stringifyJsonWithBigints(cached[field]);
    const currentStr = stringifyJsonWithBigints(current[field]);

    if (cachedStr !== currentStr) {
      // Field differs - is it in staleOn?
      if (!staleOn.includes(field)) {
        return false; // Non-staleOn field changed → need fresh data
      }
    }
  }

  // Also check for fields in cached that aren't in current (removed fields)
  for (const field of Object.keys(cached)) {
    if (!(field in current)) {
      if (!staleOn.includes(field)) {
        return false;
      }
    }
  }

  return true; // Only staleOn fields changed → stale OK
}
