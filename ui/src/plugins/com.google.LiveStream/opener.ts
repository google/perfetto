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

import {z} from 'zod';
import {getErrorMessage} from '../../base/errors';
import type {App} from '../../public/app';
import {fetchSeed, fetchTargets, HttpError} from './api';
import {Backoff, isTransient, sleep} from './retry';
import {targetShortName} from './target_spec';
import {TARGET, type Target, type TargetsResponse} from './wire';

/** Id of the plugin, shared by its modules. */
export const PLUGIN_ID = 'com.google.LiveStream';

/** Plugin args of a live trace, set by openLiveTrace(). */
export const LIVE_TRACE_ARGS = z.object({
  target: TARGET,
  // Whether the previous seed's clock didn't match the stream.
  epochRetry: z.boolean().default(false),
  // The position in the stream that the seed's data reach: empty and '0' if
  // the server sends none.
  streamId: z.string().default(''),
  seedSeq: z.string().regex(/^\d+$/).default('0'),
});

/** Parsed LIVE_TRACE_ARGS. */
export type LiveTraceArgs = z.infer<typeof LIVE_TRACE_ARGS>;

/**
 * Why an attempt to open a trace failed and will be retried: the target has
 * no data yet, the server fails to reach the target, or the server is
 * unreachable or failing.
 */
export type RetryKind = 'waiting' | 'failing' | 'unreachable';

/** Observes openLiveTrace(). */
export interface OpenObserver {
  // Called once the server resolved the target.
  onTargets?(targets: TargetsResponse): void;
  // Called with each failed attempt that will be retried.
  onRetry(message: string, kind: RetryKind): void;
  // Called right before the trace changes, e.g. to update the URL.
  beforeOpen?(target: Target): void;
}

/** Options of openLiveTrace(). */
export interface OpenOptions {
  // History to load, see fetchSeed().
  readonly historySec: number;
  // Whether the previous seed's clock didn't match the stream.
  readonly epochRetry?: boolean;
}

/**
 * Opens the live trace of `target`: the server's seed trace, which holds the
 * history and sets the clock and time zone, with plugin args that start
 * streaming once it loads. Retries while the server is unavailable, e.g.
 * before its first data. Rejects with `signal.reason` when `signal` aborts
 * before the trace changes.
 */
export async function openLiveTrace(
  app: App,
  target: string,
  signal: AbortSignal,
  observer: OpenObserver,
  {historySec, epochRetry = false}: OpenOptions,
): Promise<void> {
  const unreachable = (e: string) => {
    observer.onRetry(`Cannot reach the live server: ${e}`, 'unreachable');
  };
  const targets = await withRetries(
    () => fetchTargets(target, signal),
    signal,
    unreachable,
  );
  const selected = targets.selected;
  if (selected === undefined || selected.id === '') {
    throw new Error(`The live server has no target "${target}"`);
  }
  observer.onTargets?.(targets);

  // Until the target has data, the server answers 503, or 502 with the error
  // while it fails to reach the target.
  const name = targetShortName(selected);
  const seed = await withRetries(
    () => fetchSeed(selected.id, historySec, signal),
    signal,
    (e, error) => {
      const status = error instanceof HttpError ? error.status : undefined;
      if (status === 503) {
        observer.onRetry(`Waiting for ${name}: ${e}`, 'waiting');
      } else if (status === 502) {
        observer.onRetry(e, 'failing');
      } else {
        unreachable(e);
      }
    },
  );
  signal.throwIfAborted();

  const args: z.input<typeof LIVE_TRACE_ARGS> = {
    target: selected,
    epochRetry,
    streamId: seed.streamId,
    seedSeq: seed.seq.toString(),
  };
  observer.beforeOpen?.(selected);
  await app.openTraceFromBuffer({
    buffer: seed.buffer,
    title: targets.page_title ? `${targets.page_title}: ${name}` : name,
    fileName: 'live_trace.pftrace',
    pluginArgs: {[PLUGIN_ID]: args},
    // A seed is only a starting point: reloading opens a new one.
    cacheable: false,
    shareable: false,
    downloadable: false,
  });
}

// Calls `fn` until it succeeds or fails permanently. Rejects with
// `signal.reason` once `signal` aborts.
async function withRetries<T>(
  fn: () => Promise<T>,
  signal: AbortSignal,
  onRetry: (message: string, error: unknown) => void,
): Promise<T> {
  const backoff = new Backoff();
  for (;;) {
    try {
      return await fn();
    } catch (e) {
      signal.throwIfAborted();
      if (!isTransient(e)) throw e;
      onRetry(getErrorMessage(e), e);
      await sleep(backoff.next(), signal);
      signal.throwIfAborted();
    }
  }
}
