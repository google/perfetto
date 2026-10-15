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

import {STREAM_RESPONSE, TARGETS_RESPONSE} from '../wire';
import {DemoBackend, type DemoOptions, type DemoResponse} from './demo_backend';

const TRAINER = 'job-1/trainer';
const WAITING = 'Waiting for the first snapshot';

let backend: DemoBackend;

function start(options: Partial<DemoOptions> = {}) {
  backend = new DemoBackend({tz: 'UTC', backfill: 10, ...options});
}

beforeEach(() => {
  vi.useFakeTimers({now: Date.UTC(2026, 0, 1)});
});

afterEach(() => {
  backend.dispose();
  vi.useRealTimers();
});

// Answers `path`, advancing the fake clock while it long-polls.
async function get(path: string): Promise<DemoResponse> {
  const reply = backend.handle(path);
  if (reply === undefined) throw new Error(`not a demo route: ${path}`);
  let response: DemoResponse | undefined;
  void reply.then((r) => (response = r));
  await vi.advanceTimersByTimeAsync(0);
  while (response === undefined) await vi.advanceTimersByTimeAsync(250);
  return response;
}

function json(response: DemoResponse): unknown {
  return JSON.parse(new TextDecoder().decode(response.body));
}

async function stream(params: {
  stream_id?: string;
  since_seq?: bigint;
  window?: string;
}) {
  const query = new URLSearchParams({target: TRAINER});
  for (const [name, value] of Object.entries(params)) {
    query.set(name, String(value));
  }
  const response = await get(`/api/stream?${query}`);
  expect(response.status).toBe(200);
  return STREAM_RESPONSE.parse(json(response));
}

type Fields = Map<number, Array<number | Uint8Array>>;

// The fields of a protobuf message: varints (exact up to 2^53), doubles and
// bytes.
function fields(buf: Uint8Array): Fields {
  const out: Fields = new Map();
  const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  let pos = 0;
  const varint = () => {
    let value = 0;
    for (let scale = 1; ; scale *= 128) {
      const byte = buf[pos++];
      value += (byte & 0x7f) * scale;
      if (byte < 0x80) return value;
    }
  };
  while (pos < buf.length) {
    const tag = varint();
    let value: number | Uint8Array;
    if (tag % 8 === 0) {
      value = varint();
    } else if (tag % 8 === 1) {
      value = view.getFloat64(pos, true);
      pos += 8;
    } else {
      const length = varint();
      value = buf.subarray(pos, pos + length);
      pos += length;
    }
    const list = out.get(Math.floor(tag / 8)) ?? [];
    list.push(value);
    out.set(Math.floor(tag / 8), list);
  }
  return out;
}

function int(f: Fields, field: number): number | undefined {
  const value = f.get(field)?.[0];
  return typeof value === 'number' ? value : undefined;
}

function bytes(f: Fields, field: number): Uint8Array | undefined {
  const value = f.get(field)?.[0];
  return value instanceof Uint8Array ? value : undefined;
}

// The slice starts and counter points of a seed trace, as sorted
// "<name>@<ts>" and "<track id>@<ts>".
function seedEvents(seed: Uint8Array) {
  const text = (b?: Uint8Array) => new TextDecoder().decode(b);
  const tracks = new Map<number | undefined, string>();
  const slices: string[] = [];
  const points: string[] = [];
  for (const raw of fields(seed).get(1) ?? []) {
    if (typeof raw === 'number') continue;
    const packet = fields(raw);
    const descriptor = bytes(packet, 60);
    if (descriptor !== undefined) {
      const d = fields(descriptor);
      const id = /track_id=(.*)/.exec(text(bytes(d, 14)))?.[1];
      tracks.set(int(d, 1), id ?? '');
    }
    const event = bytes(packet, 11);
    if (event === undefined) continue;
    const e = fields(event);
    const ts = int(packet, 8);
    const type = int(e, 9);
    if (type === 4) points.push(`${tracks.get(int(e, 11))}@${ts}`);
    if (type === 1 || type === 3) slices.push(`${text(bytes(e, 23))}@${ts}`);
  }
  return {slices: slices.sort(), points: points.sort()};
}

test('api/targets: the default target, tasks and long names', async () => {
  start();
  const targets = TARGETS_RESPONSE.parse(json(await get('/api/targets')));
  expect(targets.selected?.id).toBe(TRAINER);
  expect(targets.targets.some((t) => t.task_count > 1)).toBe(true);
  expect(targets.targets.some((t) => t.id.length > 40)).toBe(true);

  const task = await get(`/api/targets?target=${TRAINER}/2`);
  const selected = TARGETS_RESPONSE.parse(json(task)).selected;
  expect([selected?.id, selected?.task_index]).toEqual([`${TRAINER}/2`, 2]);
  expect((await get(`/api/targets?target=${TRAINER}/9`)).status).toBe(404);
});

test('a snapshot, then deltas that complete active slices by key', async () => {
  start();
  const snapshot = await stream({});
  expect(snapshot.full).toBe(true);
  expect(snapshot.tracks.map((t) => t.name)).toContain('Errors');
  expect(snapshot.counter_points.some((p) => p.track_id === 'step')).toBe(true);
  const seen = new Set(snapshot.completed_slices.map((s) => s.key));

  const active = snapshot.active_slices[0];
  let resp = snapshot;
  for (let i = 0; i < 100; i++) {
    resp = await stream({stream_id: resp.stream_id, since_seq: resp.seq});
    expect(resp.full).toBe(false);
    for (const slice of resp.completed_slices) {
      expect(seen.has(slice.key)).toBe(false);
      seen.add(slice.key);
    }
    const done = resp.completed_slices.find((s) => s.key === active.key);
    if (done !== undefined) {
      expect([done.track_id, done.ts_ns]).toEqual([
        active.track_id,
        active.ts_ns,
      ]);
      return;
    }
  }
  throw new Error(`${active.key} never completed`);
});

test('pages of up to page_size entries add up to the delta', async () => {
  start();
  const first = await stream({});
  await vi.advanceTimersByTimeAsync(5000);
  backend.set([['stall', '1']]); // Freezes the stream.
  const whole = await stream({
    stream_id: first.stream_id,
    since_seq: first.seq,
  });
  expect(whole.more).toBe(false);

  backend.set([['page_size', '50']]);
  const pages = [];
  let resp = first;
  do {
    resp = await stream({stream_id: resp.stream_id, since_seq: resp.seq});
    pages.push(resp);
  } while (resp.more);
  const entries = (r: typeof resp) => [
    ...r.completed_slices.map((s) => s.key),
    ...r.counter_points.map((p) => `${p.track_id}@${p.ts_ns}`),
  ];
  expect(pages.length).toBeGreaterThan(1);
  expect(pages.every((p) => !p.full && entries(p).length <= 50)).toBe(true);
  expect(resp.seq).toBe(whole.seq);
  expect(pages.flatMap(entries).sort()).toEqual(entries(whole).sort());

  const other = await stream({stream_id: 'other', since_seq: resp.seq});
  expect(other.full).toBe(true);
});

test('a delta from an evicted change is a snapshot', async () => {
  start({history: 10});
  const first = await stream({});
  await vi.advanceTimersByTimeAsync(15_000);
  const late = await stream({stream_id: first.stream_id, since_seq: first.seq});
  expect(late.full).toBe(true);
  const next = await stream({stream_id: late.stream_id, since_seq: late.seq});
  expect(next.full).toBe(false);
});

test('demo/restart: a new stream, waiting for its first data', async () => {
  start();
  const first = await stream({});
  backend.set([['stall', '1']]);
  const restart = await get(`/demo/restart?target=${TRAINER}`);
  expect(json(restart)).toEqual({restarted: [TRAINER]});

  const waiting = await stream({
    stream_id: first.stream_id,
    since_seq: first.seq,
  });
  expect(waiting.stream_id).not.toBe(first.stream_id);
  expect([waiting.full, waiting.seq, waiting.last_error]).toEqual([
    true,
    0n,
    '',
  ]);
  const seed = await get(`/api/seed?target=${TRAINER}`);
  expect([seed.status, json(seed)]).toEqual([
    503,
    {error: `no data from ${TRAINER} yet`},
  ]);

  // Errors before data say that none came yet.
  backend.set([['error', 'Unreachable']]);
  const failed = await stream({stream_id: waiting.stream_id, since_seq: 0n});
  expect(failed.last_error).toBe(`${WAITING}: Unreachable`);
  const failedSeed = await get(`/api/seed?target=${TRAINER}`);
  expect([failedSeed.status, json(failedSeed)]).toEqual([
    502,
    {error: `${WAITING}: Unreachable`},
  ]);

  backend.set([
    ['stall', '0'],
    ['error', ''],
  ]);
  const live = await stream({stream_id: failed.stream_id, since_seq: 0n});
  expect([live.seq, live.last_error]).toEqual([1n, '']);
});

test('stall holds data; errors show at once', async () => {
  start();
  const first = await stream({});
  backend.set([['stall', '1']]);
  const t0 = Date.now();
  const held = await stream({stream_id: first.stream_id, since_seq: first.seq});
  expect(Date.now() - t0).toBeGreaterThanOrEqual(10_000);
  expect([held.seq, held.completed_slices.length]).toEqual([first.seq, 0]);

  backend.set([['error', 'connection refused']]);
  const failed = await stream({stream_id: held.stream_id, since_seq: held.seq});
  expect(failed.last_error).toBe('connection refused');
  expect(failed.seq).toBe(held.seq + 1n);
});

test('api/seed: stream position headers, or 503 without data', async () => {
  start();
  const resp = await stream({});
  const seed = await get(`/api/seed?target=${TRAINER}`);
  expect(seed.status).toBe(200);
  expect(seed.headers['X-Live-Stream-Id']).toBe(resp.stream_id);
  expect(seed.headers['X-Live-Seq']).toBe(String(resp.seq));
  expect(seed.headers['Content-Disposition']).toBeUndefined();

  const download = await get(`/api/download?target=${TRAINER}`);
  expect(download.headers['Content-Disposition']).toMatch(/^attachment/);
  backend.set([['seed_status', '500']]);
  expect((await get(`/api/seed?target=${TRAINER}`)).status).toBe(500);
  backend.dispose();

  start({stall: true});
  const waiting = await get(`/api/seed?target=${TRAINER}`);
  expect(waiting.status).toBe(503);
});

test('api/seed: history_sec of history, all errors, earlier points', async () => {
  start({backfill: 120});
  await stream({});
  backend.set([['stall', '1']]); // Freezes the stream.
  const all = await stream({window: '120'});

  // As a live server: the window ends at the latest data.
  const end = (s: {ts_ns: bigint; dur_ns: bigint}) => s.ts_ns + s.dur_ns;
  const times = [
    ...all.completed_slices.map(end),
    ...all.counter_points.map((p) => p.ts_ns),
  ];
  const from = times.reduce((a, b) => (a > b ? a : b)) - 10_000_000_000n;
  const slices = all.completed_slices.filter(
    (s) => s.category === 'error' || end(s) >= from,
  );
  const previous = new Map<string, bigint>();
  for (const p of all.counter_points) {
    const last = previous.get(p.track_id) ?? -1n;
    if (p.ts_ns < from && p.ts_ns > last) previous.set(p.track_id, p.ts_ns);
  }
  const points = all.counter_points.filter(
    (p) => p.ts_ns >= (previous.get(p.track_id) ?? from),
  );
  // The window leaves out slices, and keeps errors and points from before.
  expect(slices.length).toBeLessThan(all.completed_slices.length);
  expect(slices.some((s) => s.category === 'error' && end(s) < from)).toBe(
    true,
  );
  expect(previous.size).toBeGreaterThan(0);

  const seed = async (query: string) => {
    const response = await get(`/api/seed?target=${TRAINER}${query}`);
    return seedEvents(response.body);
  };
  const names = (list: typeof slices) =>
    list.map((s) => `${s.name}@${s.ts_ns}`).sort();
  expect((await seed('')).slices).toEqual(names(all.completed_slices));
  const windowed = await seed('&history_sec=10');
  expect(windowed.slices).toEqual(names(slices));
  expect(windowed.points).toEqual(
    points.map((p) => `${p.track_id}@${p.ts_ns}`).sort(),
  );
});

test('equal seeds, equal slices', async () => {
  const completed = async (seed: number) => {
    start({seed});
    const resp = await stream({});
    backend.dispose();
    return resp.completed_slices;
  };
  const a = await completed(1);
  expect(a.length).toBeGreaterThan(0);
  expect(await completed(1)).toEqual(a);
  expect(await completed(2)).not.toEqual(a);
});

test('rejects bad requests and settings', async () => {
  start();
  expect(backend.handle('/index.html')).toBeUndefined();
  const status = async (path: string) => (await get(path)).status;
  expect(await status('/api/nope')).toBe(404);
  expect(await status('/api/stream?target=nope')).toBe(404);
  expect(await status(`/api/stream?target=${TRAINER}&since_seq=-1`)).toBe(400);
  expect(await status(`/api/stream?target=${TRAINER}&window=x`)).toBe(400);
  for (const value of ['0', '-1', '1.5', '', 'soon', '9'.repeat(20)]) {
    const seed = `/api/seed?target=${TRAINER}&history_sec=${value}`;
    expect(await status(seed)).toBe(400);
  }
  expect(await status('/demo/set?rate=0')).toBe(400);
  expect(await status('/demo/set?lanes=17')).toBe(400);

  expect(
    backend.set([
      ['rate', '2'],
      ['lanes', 'x'],
    ]),
  ).toBe('bad lanes: "x"');
  expect(backend.options.rate).toBe(1);
  expect(backend.set([['nope', '1']])).toBe('unknown setting "nope"');

  backend.dispose();
  expect(await status('/api/targets')).toBe(503);
});
