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

// Client of the live server's HTTP API. URLs are relative to the page, which
// the live server serves.

import {z} from 'zod';
import {
  ERROR_RESPONSE,
  STREAM_RESPONSE,
  TARGETS_RESPONSE,
  type StreamResponse,
  type TargetsResponse,
} from './wire';

// Above the 20 s that a busy server may take to answer a stream request: a
// 10 s long-poll, then a wait for a response slot.
const REQUEST_TIMEOUT_MS = 30_000;

// Seeds and downloads carry hours of data.
const LARGE_BODY_TIMEOUT_MS = 120_000;

/** A non-2xx response. */
export class HttpError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
    this.name = 'HttpError';
  }
}

/** A response body that doesn't match the wire schema. */
export class InvalidResponseError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'InvalidResponseError';
  }
}

/** Parameters of fetchStream(). */
export interface StreamRequest {
  readonly target: string;
  // Empty for the first request of a session.
  readonly streamId: string;
  readonly sinceSeq: bigint;
  // History requested in full responses, in seconds.
  readonly windowSec: number;
}

/** Fetches the targets and the selected one: `target`, or the default. */
export async function fetchTargets(
  target: string,
  signal: AbortSignal,
): Promise<TargetsResponse> {
  // Without a target the server selects its default one.
  const query = target === '' ? '' : `?${new URLSearchParams({target})}`;
  const json = await request(`api/targets${query}`, signal, (r) => r.json());
  return parse(TARGETS_RESPONSE, json, 'targets');
}

/** A seed trace, and the position in the stream that its data reach. */
export interface Seed {
  readonly buffer: ArrayBuffer;
  // Empty, and seq 0, if the server sends no position: its seeds hold no
  // data.
  readonly streamId: string;
  readonly seq: bigint;
}

/**
 * Fetches the seed trace: the target's latest `historySec` of history, which
 * also sets the clock and time zone of the live trace.
 */
export function fetchSeed(
  target: string,
  historySec: number,
  signal: AbortSignal,
): Promise<Seed> {
  const query = new URLSearchParams({
    target,
    history_sec: historySec.toString(),
  });
  return request(
    `api/seed?${query}`,
    signal,
    async (r) => {
      const seq = r.headers.get('X-Live-Seq') ?? '';
      return {
        buffer: await r.arrayBuffer(),
        streamId: r.headers.get('X-Live-Stream-Id') ?? '',
        seq: /^\d+$/.test(seq) ? BigInt(seq) : 0n,
      };
    },
    LARGE_BODY_TIMEOUT_MS,
  );
}

/** Long-polls for changes after `sinceSeq`. */
export async function fetchStream(
  req: StreamRequest,
  signal: AbortSignal,
): Promise<StreamResponse> {
  const query = new URLSearchParams({
    target: req.target,
    stream_id: req.streamId,
    since_seq: req.sinceSeq.toString(),
    window: req.windowSec.toString(),
  });
  const json = await request(`api/stream?${query}`, signal, (r) => r.json());
  return parse(STREAM_RESPONSE, json, 'stream response');
}

/** Fetches all data retained by the server as a Perfetto trace. */
export function fetchDownload(
  target: string,
  signal: AbortSignal,
): Promise<Blob> {
  return request(
    `api/download?${new URLSearchParams({target})}`,
    signal,
    (r) => r.blob(),
    LARGE_BODY_TIMEOUT_MS,
  );
}

// Fetches `url` and reads the body of a 2xx response with `read`. Throws
// HttpError for other responses, and the reason of `signal` once aborted.
// The timeout covers the whole exchange, body included.
async function request<T>(
  url: string,
  signal: AbortSignal,
  read: (resp: Response) => Promise<T>,
  timeoutMs = REQUEST_TIMEOUT_MS,
): Promise<T> {
  signal.throwIfAborted();

  // Aborts the request when the caller aborts or on timeout, and always on
  // exit, so that an unread body doesn't keep a connection busy.
  const controller = new AbortController();
  const onAbort = () => controller.abort(signal.reason);
  signal.addEventListener('abort', onAbort);
  const timer = setTimeout(() => {
    controller.abort(new Error(`Request timed out after ${timeoutMs} ms`));
  }, timeoutMs);
  try {
    const resp = await fetch(url, {signal: controller.signal});
    if (!resp.ok) {
      throw new HttpError(resp.status, await readError(resp));
    }
    return await read(resp);
  } finally {
    clearTimeout(timer);
    signal.removeEventListener('abort', onAbort);
    controller.abort();
  }
}

// Returns the message of an ErrorResponse body, if any.
async function readError(resp: Response): Promise<string> {
  // Proxies may answer with non-JSON bodies.
  const json: unknown = await resp.json().catch(() => undefined);
  const parsed = ERROR_RESPONSE.safeParse(json);
  if (parsed.success && parsed.data.error !== '') {
    return parsed.data.error;
  }
  return `HTTP ${resp.status} ${resp.statusText}`.trim();
}

function parse<T>(schema: z.ZodType<T>, json: unknown, what: string): T {
  const result = schema.safeParse(json);
  if (!result.success) {
    throw new InvalidResponseError(
      `Invalid ${what} from the live server: ${z.prettifyError(result.error)}`,
    );
  }
  return result.data;
}
