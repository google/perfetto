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

// Per-engine cache for diff fetcher results.
//
// Diff views run the same `(upid, graph_sample_ts)`-filtered class /
// dominators aggregation on each side of the diff. With a long-running
// primary trace the user typically swaps the baseline a few times — at
// 7+ s per ClassesDiff load on a 60 MB hprof that adds up quickly. The
// primary's side is the same across baseline swaps; caching it keyed by
// (engine, dump) makes the second visit instantaneous.
//
// Two design choices worth noting:
//
//   * The outer map is a WeakMap keyed by Engine, so a disposed baseline
//     engine takes its cached rows with it without any explicit cleanup
//     call from the baseline-pool teardown path.
//   * The value is a Promise, not a resolved array, so concurrent
//     fetches (Promise.all of baseline+current on first render of a
//     new tab) share the in-flight query rather than dispatching twice.
//
// On rejection we drop the entry so the next call retries instead of
// re-serving a stale failure.

import type {Engine} from '../../../trace_processor/engine';
import type {Row} from '../../../trace_processor/query_result';

type Cache = Map<string, Promise<ReadonlyArray<Row>>>;

const PER_ENGINE_CACHE: WeakMap<Engine, Cache> = new WeakMap();

export async function cachedFetch(
  engine: Engine,
  key: string,
  fetcher: () => Promise<ReadonlyArray<Row>>,
): Promise<ReadonlyArray<Row>> {
  let perEngine = PER_ENGINE_CACHE.get(engine);
  if (!perEngine) {
    perEngine = new Map();
    PER_ENGINE_CACHE.set(engine, perEngine);
  }
  const existing = perEngine.get(key);
  if (existing !== undefined) return existing;
  const promise = fetcher().catch((err) => {
    // Drop failed entries so the next caller retries instead of being
    // permanently stuck on a stale failure.
    if (perEngine!.get(key) === promise) perEngine!.delete(key);
    throw err;
  });
  perEngine.set(key, promise);
  return promise;
}

// Stable cache-key fragment for a heap dump. Combines (upid, ts) into
// a string that survives across re-mounts of a diff view (Mithril
// instances are short-lived; the cache is module-scope).
export function dumpKey(upid: number, ts: number | bigint): string {
  return `${upid}:${ts}`;
}
