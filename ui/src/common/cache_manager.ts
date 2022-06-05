// Copyright (C) 2021 The Android Open Source Project
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
 * This file deals with caching traces in the browser's Cache storage. The
 * traces are cached so that the UI can gracefully reload a trace when the tab
 * containing it is discarded by Chrome(e.g. because the tab was not used for a
 * long time) or when the user accidentally hits reload.
 */
import {assertExists} from '../base/logging';
import {TraceArrayBufferSource, TraceSource} from './state';

const TRACE_CACHE_NAME = 'cached_traces';
const TRACE_CACHE_SIZE = 10;

export async function cacheTrace(
    traceSource: TraceSource, traceUuid: string): Promise<boolean> {
  let trace;
  let title = '';
  let fileName = '';
  let url = '';
  let contentLength = 0;
  let localOnly = false;
  switch (traceSource.type) {
    case 'ARRAY_BUFFER':
      trace = traceSource.buffer;
      title = traceSource.title;
      fileName = traceSource.fileName || '';
      url = traceSource.url || '';
      contentLength = traceSource.buffer.byteLength;
      localOnly = traceSource.localOnly || false;
      break;
    case 'FILE':
      trace = await traceSource.file.arrayBuffer();
      title = traceSource.file.name;
      contentLength = traceSource.file.size;
      break;
    default:
      return false;
  }
  assertExists(trace);

  const headers = new Headers([
    ['x-trace-title', title],
    ['x-trace-url', url],
    ['x-trace-filename', fileName],
    ['x-trace-local-only', `${localOnly}`],
    ['content-type', 'application/octet-stream'],
    ['content-length', `${contentLength}`],
    [
      'expires',
      // Expires in a week from now (now = upload time)
      (new Date((new Date()).getTime() + (1000 * 60 * 60 * 24 * 7)))
          .toUTCString(),
    ],
  ]);
  const traceCache = await caches.open(TRACE_CACHE_NAME);
  await deleteStaleEntries(traceCache);
  await traceCache.put(
      `/_${TRACE_CACHE_NAME}/${traceUuid}`, new Response(trace, {headers}));
  return true;
}

export async function tryGetTrace(traceUuid: string):
    Promise<TraceArrayBufferSource|undefined> {
  await deleteStaleEntries(await caches.open(TRACE_CACHE_NAME));
  const response = await caches.match(
      `/_${TRACE_CACHE_NAME}/${traceUuid}`, {cacheName: TRACE_CACHE_NAME});

  if (!response) return undefined;
  return {
    type: 'ARRAY_BUFFER',
    buffer: await response.arrayBuffer(),
    title: response.headers.get('x-trace-title') || '',
    fileName: response.headers.get('x-trace-filename') || undefined,
    url: response.headers.get('x-trace-url') || undefined,
    uuid: traceUuid,
    localOnly: response.headers.get('x-trace-local-only') === 'true',
  };
}

async function deleteStaleEntries(traceCache: Cache) {
  /*
   * Loop through stored caches and invalidate all but the most recent 10.
   */
  const keys = await traceCache.keys();
  const storedTraces: Array<{key: Request, date: Date}> = [];
  for (const key of keys) {
    const existingTrace = assertExists(await traceCache.match(key));
    const expiryDate =
        new Date(assertExists(existingTrace.headers.get('expires')));
    if (expiryDate < new Date()) {
      await traceCache.delete(key);
    } else {
      storedTraces.push({key, date: expiryDate});
    }
  }

  if (storedTraces.length <= TRACE_CACHE_SIZE) return;

  /*
   * Sort the traces descending by time, such that most recent ones are placed
   * at the beginning. Then, take traces from TRACE_CACHE_SIZE onwards and
   * delete them from cache.
   */
  const oldTraces =
      storedTraces.sort((a, b) => b.date.getTime() - a.date.getTime())
          .slice(TRACE_CACHE_SIZE);
  for (const oldTrace of oldTraces) {
    await traceCache.delete(oldTrace.key);
  }
}
