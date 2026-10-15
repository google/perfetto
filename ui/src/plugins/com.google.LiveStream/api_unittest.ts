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

import {
  fetchSeed,
  fetchStream,
  fetchTargets,
  HttpError,
  InvalidResponseError,
  type StreamRequest,
} from './api';

const REQUEST: StreamRequest = {
  target: 'job-1/worker/3',
  streamId: 's1',
  sinceSeq: 6n,
  windowSec: 120,
};

const fetchMock = vi.fn<typeof fetch>();

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {status});
}

function signal(): AbortSignal {
  return new AbortController().signal;
}

// A fetch that never responds, and rejects when aborted.
function hangingFetch(_url: unknown, init?: RequestInit): Promise<Response> {
  return new Promise((_resolve, reject) => {
    const fetchSignal = init?.signal;
    fetchSignal?.addEventListener('abort', () => reject(fetchSignal.reason));
  });
}

// A fetch whose body never completes, and fails when aborted.
async function stalledBodyFetch(
  _url: unknown,
  init?: RequestInit,
): Promise<Response> {
  const fetchSignal = init?.signal;
  const body = new ReadableStream({
    start(controller) {
      fetchSignal?.addEventListener('abort', () =>
        controller.error(fetchSignal.reason),
      );
    },
  });
  return new Response(body);
}

beforeEach(() => {
  fetchMock.mockReset();
  vi.stubGlobal('fetch', fetchMock);
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

test('requests changes since the last response', async () => {
  fetchMock.mockResolvedValue(jsonResponse({stream_id: 's1', seq: '7'}));

  const resp = await fetchStream(REQUEST, signal());

  expect(fetchMock.mock.calls[0][0]).toBe(
    'api/stream?target=job-1%2Fworker%2F3&stream_id=s1&since_seq=6&window=120',
  );
  expect(resp.seq).toBe(7n);
});

test('lets the server select the default target', async () => {
  fetchMock.mockResolvedValue(jsonResponse({}));

  await fetchTargets('', signal());

  expect(fetchMock.mock.calls[0][0]).toBe('api/targets');
});

test('requests the seed with its history', async () => {
  const headers = {'X-Live-Stream-Id': 's1', 'X-Live-Seq': '42'};
  fetchMock.mockResolvedValue(new Response(new ArrayBuffer(4), {headers}));

  const seed = await fetchSeed('job-1/worker', 3600, signal());

  expect(fetchMock.mock.calls[0][0]).toBe(
    'api/seed?target=job-1%2Fworker&history_sec=3600',
  );
  expect(seed).toMatchObject({streamId: 's1', seq: 42n});
  expect(seed.buffer.byteLength).toBe(4);
});

test('reports the error message of the server', async () => {
  fetchMock.mockResolvedValue(jsonResponse({error: 'no data yet'}, 503));

  const error = await fetchStream(REQUEST, signal()).catch((e) => e);

  expect(error).toBeInstanceOf(HttpError);
  expect(error).toMatchObject({status: 503, message: 'no data yet'});
});

test('reports the status of other errors', async () => {
  const html = new Response('<html>', {status: 502, statusText: 'Bad Gateway'});
  fetchMock.mockResolvedValue(html);

  await expect(fetchStream(REQUEST, signal())).rejects.toMatchObject({
    status: 502,
    message: 'HTTP 502 Bad Gateway',
  });
});

test('rejects responses that do not match the schema', async () => {
  fetchMock.mockResolvedValue(jsonResponse({seq: 'x'}));

  await expect(fetchStream(REQUEST, signal())).rejects.toBeInstanceOf(
    InvalidResponseError,
  );
});

test('aborts the request when the caller aborts', async () => {
  fetchMock.mockImplementation(hangingFetch);
  const controller = new AbortController();

  const resp = fetchStream(REQUEST, controller.signal);
  controller.abort(new Error('paused'));

  await expect(resp).rejects.toThrow('paused');
});

test('cancels requests that time out', async () => {
  vi.useFakeTimers();
  fetchMock.mockImplementation(hangingFetch);

  const resp = fetchStream(REQUEST, signal());
  const rejected = expect(resp).rejects.toThrow('timed out');

  // A busy server may answer after 20 s.
  await vi.advanceTimersByTimeAsync(20_000);
  expect(fetchMock.mock.calls[0][1]?.signal?.aborted).toBe(false);

  await vi.advanceTimersByTimeAsync(10_000);
  await rejected;
  expect(fetchMock.mock.calls[0][1]?.signal?.aborted).toBe(true);
});

test('times out while reading the body', async () => {
  vi.useFakeTimers();
  fetchMock.mockImplementation(stalledBodyFetch);

  const resp = fetchStream(REQUEST, signal());
  const rejected = expect(resp).rejects.toThrow('timed out');
  await vi.advanceTimersByTimeAsync(30_000);

  await rejected;
});
