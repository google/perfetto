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

import {HighPrecisionTime} from '../../base/high_precision_time';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {errResult, okResult, type Result} from '../../base/result';
import {Time} from '../../base/time';
import type {MinimapContentProvider} from '../../public/minimap';
import type {Selection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {Workspace} from '../../public/workspace';
import type {QueryResult} from '../../trace_processor/query_result';
import type {LiveHost, LiveSessionModel} from './controller';
import {LIVE_TRACE_ARGS} from './opener';
import {LiveSession} from './session';
import {type LiveSettings, registerLiveSettings} from './settings';
import {queryResult} from './test_utils';
import type {Target} from './wire';

const EPOCH_NS = 1_760_000_000_000_000_000n;
const TARGET = {id: 'job-1/worker'};
const OTHER_TARGET = {id: 'job-1/eval', task_count: 4};
const TRACKS = [{id: 'tasks', name: 'Tasks', kind: 'KIND_SLICE'}];

// The minimap's bucket width, see BusyTime.
const BUCKET_NS = 1n << 30n;

// The history of a seed ends here, where its data reach seq 30 of stream s1.
const SEED_END = Time.fromRaw(30_000_000_000n);

// A stream response of stream `streamId` with data up to `seq`.
function data(seq: number, streamId = 's1') {
  return {
    stream_id: streamId,
    seq: `${seq}`,
    start_epoch_ns: `${EPOCH_NS}`,
    playhead_ns: `${seq}000000000`,
  };
}

// A completed slice of TRACKS.
function slice(key: string, tsNs = 1, durNs = 1) {
  return {key, track_id: 'tasks', ts_ns: `${tsNs}`, dur_ns: `${durNs}`};
}

// Answers stream requests with the queued responses, then waits until the
// request aborts. Answers target requests with TARGET and OTHER_TARGET.
class FakeServer {
  readonly streamRequests: URLSearchParams[] = [];
  readonly signals: AbortSignal[] = [];
  private readonly responses: object[] = [];

  respond(...responses: object[]) {
    this.responses.push(...responses);
  }

  readonly fetch = async (
    input: RequestInfo | URL,
    init?: RequestInit,
  ): Promise<Response> => {
    const [route, query] = String(input).split('?');
    if (route === 'api/targets') {
      return json({targets: [TARGET, OTHER_TARGET], selected: TARGET});
    }

    const signal = init?.signal ?? new AbortController().signal;
    this.streamRequests.push(new URLSearchParams(query));
    this.signals.push(signal);
    const response = this.responses.shift();
    if (response !== undefined) return json(response);
    return new Promise((_resolve, reject) => {
      signal.addEventListener('abort', () => reject(signal.reason));
    });
  };

  // The stream position of each request: [stream_id, since_seq].
  positions(): string[][] {
    return this.streamRequests.map((r) => [
      r.get('stream_id') ?? '',
      r.get('since_seq') ?? '',
    ]);
  }
}

function json(body: object): Response {
  return new Response(JSON.stringify(body));
}

// Records and accepts all queries, but fails the next one when `failNext` is
// set, those that contain `failQuery`, and ROLLBACK when `failRollback` is
// set. Queries return the rows of `respond`, if any.
class FakeEngine {
  readonly queries: string[] = [];
  failNext = false;
  failQuery?: string;
  failRollback = false;
  respond: (sql: string) => QueryResult | undefined = () => undefined;

  async query(sql: string): Promise<QueryResult> {
    this.queries.push(sql);
    const fail =
      this.failNext ||
      (this.failQuery !== undefined && sql.includes(this.failQuery));
    if (fail) {
      this.failNext = false;
      throw new Error('query failed');
    }
    return this.respond(sql) ?? queryResult([]);
  }

  async tryQuery(): Promise<Result<unknown>> {
    return this.failRollback ? errResult('rollback failed') : okResult();
  }
}

// Answers the queries of a seed's import: native track 1 holds TRACKS at
// depth 0, with a slice that is busy half of the first bucket.
function seedQueries(sql: string): QueryResult | undefined {
  if (sql.includes('FROM track t')) {
    return queryResult(
      ['nativeId', 'name', 'isCounter', 'unit', 'marker'],
      [[1, 'Tasks', 0, '', 'com.google.LiveStream:depth=0;track_id=tasks']],
    );
  }
  if (sql.includes('ORDER BY end')) {
    return queryResult(['id', 'trackId', 'ts', 'end']);
  }
  if (sql.includes('UNION ALL')) return queryResult(['trackId', 'ts']);
  if (sql.includes('MAX(id)')) return queryResult(['id'], [[1]]);
  if (sql.includes('AS bucket')) {
    const busy = Number(BUCKET_NS / 2n);
    return queryResult(['trackId', 'bucket', 'busy'], [[1, 0, busy]]);
  }
  return undefined;
}

// The minimap rows of `provider` over 10 buckets, in cells of one.
function minimapRows(provider: MinimapContentProvider) {
  const span = HighPrecisionTimeSpan.fromTime(
    Time.ZERO,
    Time.fromRaw(10n * BUCKET_NS),
  );
  return provider.getData(span, BUCKET_NS);
}

// The app-level side of the plugin, see LiveController.
class FakeHost implements LiveHost {
  following = true;
  model?: LiveSessionModel;
  targets: readonly Target[] = [];
  readonly reopen = vi.fn<(target: string, epochRetry: boolean) => void>();

  constructor(readonly settings: LiveSettings) {}

  attach(model: LiveSessionModel): Disposable {
    this.model = model;
    return {
      [Symbol.dispose]: () => {
        this.model = undefined;
      },
    };
  }

  setTargets(targets: readonly Target[]) {
    this.targets = targets;
  }

  refreshStatus() {}
}

// A trace with what LiveSession uses.
function fakeTrace() {
  const engine = new FakeEngine();
  const readyListeners: Array<() => void> = [];
  const frameCallbacks: Array<() => void> = [];
  const minimapProviders: MinimapContentProvider[] = [];
  const selection: {current: Selection} = {current: {kind: 'empty'}};
  const raf = {
    addCanvasRedrawCallback: (callback: () => void) => {
      frameCallbacks.push(callback);
      return disposable;
    },
    scheduleFullRedraw: vi.fn(),
    scheduleCanvasRedraw: vi.fn(),
  };
  const disposable = {[Symbol.dispose]: () => {}};
  // Moved by the session while following.
  const timeline = {
    visibleWindow: new HighPrecisionTimeSpan(HighPrecisionTime.ZERO, 10e9),
    setVisibleWindow(span: HighPrecisionTimeSpan) {
      this.visibleWindow = span;
    },
  };
  const traceInfo = {
    unixOffset: Time.fromRaw(-EPOCH_NS),
    start: Time.ZERO,
    end: Time.ZERO,
  };
  const trace = {
    engine,
    settings: {
      register: (setting: {defaultValue: unknown}) => ({
        get: () => setting.defaultValue,
      }),
      get: () => undefined,
    },
    selection: {
      get selection() {
        return selection.current;
      },
      registerAreaSelectionTab: () => {},
    },
    minimap: {
      registerContentProvider: (provider: MinimapContentProvider) => {
        minimapProviders.push(provider);
      },
    },
    tracks: {registerTrack: () => {}},
    omnibox: {showStatusMessage: vi.fn()},
    raf,
    onTraceReady: {
      addListener: (listener: () => void) => {
        readyListeners.push(listener);
        return disposable;
      },
    },
    defaultWorkspace: new Workspace(),
    timeline,
    traceInfo,
  } as unknown as Trace;

  return {
    trace,
    engine,
    raf,
    selection,
    timeline,
    traceInfo,
    minimapProvider: () => minimapProviders[0]!,
    // Runs the callbacks of a frame.
    frame: () => frameCallbacks.forEach((callback) => callback()),
    traceReady: () => readyListeners.forEach((listener) => listener()),
  };
}

let server: FakeServer;

beforeEach(() => {
  vi.useFakeTimers({
    toFake: [
      'setTimeout',
      'clearTimeout',
      'setInterval',
      'clearInterval',
      'performance',
    ],
  });
  server = new FakeServer();
  vi.stubGlobal('fetch', vi.fn(server.fetch));
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

interface StartOptions {
  // Whether the previous seed's clock didn't match the stream.
  readonly epochRetry?: boolean;
  // The seed's history: none, as without a stream position; that of
  // seedQueries(), up to SEED_END; or one that fails to load.
  readonly history?: 'none' | 'seed' | 'failed';
}

// Starts a session of a loaded trace.
async function start({
  epochRetry = false,
  history = 'none',
}: StartOptions = {}) {
  const fake = fakeTrace();
  const host = new FakeHost(registerLiveSettings(fake.trace.settings));
  const position = history === 'none' ? {} : {streamId: 's1', seedSeq: '30'};
  const args = LIVE_TRACE_ARGS.parse({target: TARGET, epochRetry, ...position});
  if (history === 'seed') {
    fake.traceInfo.end = SEED_END;
    fake.engine.respond = seedQueries;
  }
  if (history === 'failed') fake.engine.failQuery = 'FROM track t';
  const session = new LiveSession(fake.trace, args, host);
  await session.init();
  fake.traceReady();
  await vi.advanceTimersByTimeAsync(0);
  const model = () => {
    if (host.model === undefined) throw new Error('Not attached');
    return host.model;
  };
  return {
    session,
    host,
    model,
    status: () => model().state().status,
    error: () => model().state().error,
    ...fake,
  };
}

test('polls from the position of the latest response', async () => {
  server.respond(data(5), data(6));
  await start();
  await vi.advanceTimersByTimeAsync(1_000);

  expect(server.positions()).toEqual([
    ['', '0'],
    ['s1', '5'],
    ['s1', '6'],
  ]);
});

test('requests more changes at once', async () => {
  server.respond({...data(5), more: true}, {...data(6), more: true}, data(7));
  await start();

  // Faster than the 0.25 s update interval.
  await vi.advanceTimersByTimeAsync(100);
  expect(server.positions()).toEqual([
    ['', '0'],
    ['s1', '5'],
    ['s1', '6'],
  ]);

  await vi.advanceTimersByTimeAsync(200);
  expect(server.positions().at(-1)).toEqual(['s1', '7']);
});

test('waits for the next interval when more changes bring no progress', async () => {
  server.respond({...data(5), more: true}, {...data(5), more: true});
  await start();

  await vi.advanceTimersByTimeAsync(100);
  expect(server.positions()).toEqual([
    ['', '0'],
    ['s1', '5'],
  ]);
});

test('keeps its position when a response fails to apply', async () => {
  server.respond(
    data(5),
    {...data(6), tracks: TRACKS, completed_slices: [slice('a')]},
    data(7),
  );
  const {engine} = await start();
  engine.failNext = true;

  // Retries after 1 s.
  await vi.advanceTimersByTimeAsync(2_000);

  expect(server.positions()).toEqual([
    ['', '0'],
    ['s1', '5'],
    ['s1', '5'],
    ['s1', '7'],
  ]);
});

test('stops polling when an update cannot be rolled back', async () => {
  server.respond(data(1), {
    ...data(2),
    tracks: TRACKS,
    completed_slices: [slice('a')],
  });
  const {engine, status} = await start();
  engine.failNext = true;
  engine.failRollback = true;

  await vi.advanceTimersByTimeAsync(60_000);

  expect(server.streamRequests.length).toBe(2);
  expect(status()).toBe('error');
});

test('pausing aborts the request and polls again on resume', async () => {
  const {model} = await start();
  expect(server.streamRequests.length).toBe(1);

  model().togglePause();
  expect(server.signals[0]!.aborted).toBe(true);
  await vi.advanceTimersByTimeAsync(60_000);
  expect(server.streamRequests.length).toBe(1);

  model().togglePause();
  await vi.advanceTimersByTimeAsync(0);
  expect(server.streamRequests.length).toBe(2);
});

test('following again resumes a paused stream; stopping keeps the pause', async () => {
  const {host, model, status} = await start();
  model().togglePause();
  expect(status()).toBe('paused');

  model().toggleFollow();
  expect(host.following).toBe(false);
  expect(status()).toBe('paused');

  model().toggleFollow();
  await vi.advanceTimersByTimeAsync(2_000);
  expect(host.following).toBe(true);
  expect(server.streamRequests.length).toBe(2);
  expect(status()).not.toBe('paused');
});

test('shows the end of the seed history, then polls from its position', async () => {
  const {timeline, minimapProvider} = await start({history: 'seed'});

  // 10 s wide, minus the 5 s buffer delay.
  const visible = timeline.visibleWindow;
  expect([visible.start.toTime(), visible.end.toTime()]).toEqual([
    15_000_000_000n,
    25_000_000_000n,
  ]);
  expect(server.positions()).toEqual([['s1', '30']]);
  expect(await minimapRows(minimapProvider())).toEqual([
    [{ts: 0n, dur: BUCKET_NS, load: 0.5}],
  ]);
});

test('reloads the busy time when a snapshot replaces the seed', async () => {
  // With the seed's slice again, now with a key.
  server.respond({
    ...data(31),
    full: true,
    tracks: TRACKS,
    completed_slices: [slice('a', 0, Number(BUCKET_NS / 2n))],
  });
  const {engine, minimapProvider} = await start({history: 'seed'});
  await vi.advanceTimersByTimeAsync(100);

  // From the tables, so that the slice counts once.
  const loads = engine.queries.filter((q) => q.includes('AS bucket'));
  expect(loads.length).toBe(2);
  expect(await minimapRows(minimapProvider())).toEqual([
    [{ts: 0n, dur: BUCKET_NS, load: 0.5}],
  ]);
});

test('shows a failed history load until dismissed', async () => {
  const {model, error, status} = await start({history: 'failed'});
  expect(error()).toBe('Cannot load the history: query failed');
  expect(status()).toBe('error');

  // Streams from the start instead.
  expect(server.positions()[0]).toEqual(['', '0']);

  model().dismissError('Disk full');
  expect(error()).toBe('Cannot load the history: query failed');
  model().dismissError('Cannot load the history: query failed');
  expect(error()).toBeUndefined();
});

test("shows the server's error until it clears", async () => {
  server.respond({...data(1), last_error: 'Disk full'}, data(2));
  const {error} = await start();
  await vi.advanceTimersByTimeAsync(100);
  expect(error()).toBe('Disk full');

  await vi.advanceTimersByTimeAsync(250);
  expect(error()).toBeUndefined();
});

test('buffers until there is data', async () => {
  server.respond(data(0), data(1));
  const {status, error} = await start();
  await vi.advanceTimersByTimeAsync(100);
  expect(status()).toBe('buffering');
  expect(error()).toBeUndefined();

  await vi.advanceTimersByTimeAsync(250);
  expect(status()).toBe('live');
});

test("shows the server's error before data too", async () => {
  const failing = 'Waiting for the first snapshot: Unreachable';
  server.respond({...data(0), last_error: failing}, data(1));
  const {status, error} = await start();
  await vi.advanceTimersByTimeAsync(100);
  expect(status()).toBe('error');
  expect(error()).toBe(failing);

  await vi.advanceTimersByTimeAsync(250);
  expect(status()).toBe('live');
  expect(error()).toBeUndefined();
});

test('shows the error of a restarted stream until it reopens', async () => {
  const failing = 'Waiting for the first snapshot: Unreachable';
  server.respond(
    data(5),
    {...data(0, 's2'), last_error: failing},
    data(1, 's2'),
  );
  const {host, status, error} = await start();
  await vi.advanceTimersByTimeAsync(300);
  expect(status()).toBe('error');
  expect(error()).toBe(failing);
  expect(host.reopen).not.toHaveBeenCalled();

  // The first data of the new stream reopens the trace.
  await vi.advanceTimersByTimeAsync(250);
  expect(host.reopen.mock.calls).toEqual([[TARGET.id, false]]);
});

test('redraws the page on changes only for a selected slice', async () => {
  server.respond(
    {...data(1), tracks: TRACKS},
    {...data(2), completed_slices: [slice('a')]},
    {...data(3), completed_slices: [slice('b')]},
    data(4),
  );
  const {raf, selection, traceInfo} = await start();
  // Growing the trace end redraws the page too: keep it ahead.
  traceInfo.end = Time.fromSeconds(100);
  await vi.advanceTimersByTimeAsync(100);
  raf.scheduleFullRedraw.mockClear();
  raf.scheduleCanvasRedraw.mockClear();

  // Tracks redraw from the canvas.
  await vi.advanceTimersByTimeAsync(250);
  expect(server.positions().at(-1)).toEqual(['s1', '1']);
  expect(raf.scheduleCanvasRedraw).toHaveBeenCalled();
  expect(raf.scheduleFullRedraw).not.toHaveBeenCalled();

  // The details of a selected slice may change. Slice b came 0.25 s after
  // slice a: it shows with a later response.
  selection.current = {
    kind: 'track_event',
    trackUri: 'tasks',
    eventId: 1,
    ts: Time.ZERO,
  };
  await vi.advanceTimersByTimeAsync(250);
  expect(raf.scheduleFullRedraw).not.toHaveBeenCalled();
  await vi.advanceTimersByTimeAsync(500);
  expect(raf.scheduleFullRedraw).toHaveBeenCalledTimes(1);

  // Unless nothing changed.
  await vi.advanceTimersByTimeAsync(1_000);
  expect(server.positions().at(-1)).toEqual(['s1', '4']);
  expect(raf.scheduleFullRedraw).toHaveBeenCalledTimes(1);
});

test('shows changes held back by a recent publish within a second', async () => {
  server.respond(
    {...data(1), tracks: TRACKS},
    {...data(2), completed_slices: [slice('a')]},
    {...data(3), completed_slices: [slice('b')]},
  );
  const {raf, selection} = await start();
  selection.current = {
    kind: 'track_event',
    trackUri: 'tasks',
    eventId: 1,
    ts: Time.ZERO,
  };
  await vi.advanceTimersByTimeAsync(300);
  raf.scheduleFullRedraw.mockClear();

  // Slice b came too soon after slice a, and no response follows.
  await vi.advanceTimersByTimeAsync(500);
  expect(server.positions().at(-1)).toEqual(['s1', '3']);
  expect(raf.scheduleFullRedraw).not.toHaveBeenCalled();

  await vi.advanceTimersByTimeAsync(500);
  expect(raf.scheduleFullRedraw).toHaveBeenCalledTimes(1);
});

test('redraws the canvas on new data held back by a recent publish', async () => {
  server.respond(
    {...data(1), tracks: TRACKS},
    {...data(2), completed_slices: [slice('a')]},
    {...data(3), completed_slices: [slice('b')]},
  );
  const {raf} = await start();
  await vi.advanceTimersByTimeAsync(300);
  raf.scheduleCanvasRedraw.mockClear();

  // Slice b shows with the next publish, but the playhead moves now.
  await vi.advanceTimersByTimeAsync(250);
  expect(raf.scheduleCanvasRedraw).toHaveBeenCalled();
});

test('shows the busy time of completed slices in the minimap', async () => {
  server.respond({
    ...data(1),
    tracks: TRACKS,
    completed_slices: [slice('a', 0, Number(BUCKET_NS / 2n))],
  });
  const {minimapProvider} = await start();
  await vi.advanceTimersByTimeAsync(100);

  expect(await minimapRows(minimapProvider())).toEqual([
    [{ts: 0n, dur: BUCKET_NS, load: 0.5}],
  ]);
});

test('refreshes the minimap rows as the trace grows', async () => {
  // A slice beyond the 10 buckets of the core's load.
  server.respond({
    ...data(120),
    tracks: TRACKS,
    completed_slices: [
      slice('a', Number(100n * BUCKET_NS), Number(BUCKET_NS / 2n)),
    ],
  });
  const {minimapProvider, raf} = await start();
  const rows = await minimapRows(minimapProvider());
  expect(rows).toEqual([]);
  raf.scheduleCanvasRedraw.mockClear();

  // After 5 s, for the trace up to 121 s: cells of a bucket.
  await vi.advanceTimersByTimeAsync(5_000);
  expect(rows).toEqual([[{ts: 100n * BUCKET_NS, dur: BUCKET_NS, load: 0.5}]]);
  expect(raf.scheduleCanvasRedraw).toHaveBeenCalled();
});

test('gives an empty trace span a second, for the core to load the minimap', async () => {
  // The span of a seed without history.
  const {traceInfo, raf} = await start();
  expect(traceInfo.end).toBe(Time.fromSeconds(1));
  expect(raf.scheduleFullRedraw).toHaveBeenCalled();
});

test('a new area selection stops following', async () => {
  const {host, frame, selection, model} = await start();
  const area = (end: number): Selection => ({
    kind: 'area',
    start: Time.fromRaw(0n),
    end: Time.fromRaw(BigInt(end)),
    trackUris: [],
    tracks: [],
  });
  expect(host.following).toBe(true);

  selection.current = area(10);
  frame();
  expect(host.following).toBe(false);

  // Following again keeps the area.
  model().toggleFollow();
  frame();
  expect(host.following).toBe(true);

  selection.current = area(20);
  frame();
  expect(host.following).toBe(false);
});

test('holds the timeline still while a pointer is down', async () => {
  server.respond(data(10));
  const {host, frame, timeline} = await start();
  await vi.advanceTimersByTimeAsync(100);
  frame();
  const followed = timeline.visibleWindow;

  // E.g. while dragging an area.
  window.dispatchEvent(new Event('pointerdown'));
  await vi.advanceTimersByTimeAsync(100);
  frame();
  expect(timeline.visibleWindow).toBe(followed);

  window.dispatchEvent(new Event('pointerup'));
  frame();
  expect(timeline.visibleWindow.end.gt(followed.end.toTime())).toBe(true);
  expect(host.following).toBe(true);
});

// Starts a session that follows data up to 30 s with a 10 s window.
async function startFollowing() {
  server.respond(data(30));
  const session = await start();
  await vi.advanceTimersByTimeAsync(100);
  session.frame();
  return session;
}

// Presses W on `target`, as the timeline's zoom key.
function pressW(init: KeyboardEventInit = {}, target: EventTarget = window) {
  const event = new KeyboardEvent('keydown', {code: 'KeyW', ...init});
  target.dispatchEvent(event);
}

test('advances the playhead by the time of the frame', async () => {
  const {frame, timeline} = await startFollowing();
  const frameTime = {currentTime: performance.now()};
  Object.defineProperty(document, 'timeline', {
    value: frameTime,
    configurable: true,
  });

  try {
    frame();
    const end = timeline.visibleWindow.end;

    // Callbacks of one frame see the same time, however late they run.
    await vi.advanceTimersByTimeAsync(100);
    frame();
    expect(timeline.visibleWindow.end.equals(end)).toBe(true);

    frameTime.currentTime += 100;
    frame();
    expect(timeline.visibleWindow.end.gt(end.toTime())).toBe(true);
  } finally {
    Reflect.deleteProperty(document, 'timeline');
  }
});

test('keeps following at the new width after a zoom', async () => {
  const {host, frame, timeline} = await startFollowing();
  const playhead = timeline.visibleWindow.end;

  // Zoom in, then out, around the middle, e.g. with W and S. The playhead
  // stands still, yet the window ends at it again.
  for (const widthNs of [5e9, 20e9]) {
    pressW();
    const zoomed = timeline.visibleWindow;
    timeline.setVisibleWindow(zoomed.scale(widthNs / zoomed.duration, 0.5));
    frame();

    expect(host.following).toBe(true);
    expect(timeline.visibleWindow.duration).toBeCloseTo(widthNs, -1);
    expect(timeline.visibleWindow.end.equals(playhead)).toBe(true);
  }

  // Following goes on at that width.
  await vi.advanceTimersByTimeAsync(100);
  frame();
  expect(timeline.visibleWindow.end.gt(playhead.toTime())).toBe(true);
  expect(timeline.visibleWindow.duration).toBeCloseTo(20e9, -1);
});

test('keeps following after a Ctrl+wheel zoom that moves the window', async () => {
  const {host, frame, timeline} = await startFollowing();
  const playhead = timeline.visibleWindow.end;

  // Near the start: half the width, 3 s back.
  window.dispatchEvent(new WheelEvent('wheel', {ctrlKey: true, deltaY: -50}));
  const zoomed = timeline.visibleWindow.scale(0.5, 0.2).translate(-3e9);
  timeline.setVisibleWindow(zoomed);
  frame();

  expect(host.following).toBe(true);
  expect(timeline.visibleWindow.duration).toBeCloseTo(5e9, -1);
  expect(timeline.visibleWindow.end.equals(playhead)).toBe(true);
});

test('follows the playhead when zoomed out to the whole trace', async () => {
  const {host, frame, timeline, traceInfo, model} = await startFollowing();
  const playhead = timeline.visibleWindow.end;

  // A trace from 2 s on, in a timeline that shifts windows starting before
  // it, like the core's.
  traceInfo.start = Time.fromSeconds(2);
  timeline.setVisibleWindow = (span) => {
    timeline.visibleWindow = span.fitWithin(traceInfo.start, traceInfo.end);
  };

  // Follow again, zoomed out to the whole trace.
  model().toggleFollow();
  const {start, end} = traceInfo;
  timeline.setVisibleWindow(HighPrecisionTimeSpan.fromTime(start, end));
  model().toggleFollow();
  frame();

  expect(host.following).toBe(true);
  expect(timeline.visibleWindow.start.toTime()).toBe(start);
  expect(timeline.visibleWindow.end.equals(playhead)).toBe(true);

  await vi.advanceTimersByTimeAsync(100);
  frame();
  expect(timeline.visibleWindow.end.gt(playhead.toTime())).toBe(true);
});

test('stops following after a pan, even right after zoom input', async () => {
  const {host, frame, timeline} = await startFollowing();

  // 3 s back; the width differs by rounding only.
  pressW();
  const followed = timeline.visibleWindow;
  const panned = new HighPrecisionTimeSpan(
    followed.start.subNumber(3e9),
    followed.duration + 1e-3,
  );
  timeline.setVisibleWindow(panned);
  frame();
  expect(host.following).toBe(false);

  await vi.advanceTimersByTimeAsync(100);
  frame();
  expect(timeline.visibleWindow).toBe(panned);
});

test('stops following after other width changes, e.g. F on a slice', async () => {
  const {host, frame, timeline} = await startFollowing();

  // Also a range drawn on the minimap.
  const slice = new HighPrecisionTimeSpan(
    HighPrecisionTime.ZERO.addNumber(2e9),
    1e9,
  );
  timeline.setVisibleWindow(slice);
  frame();
  expect(host.following).toBe(false);

  await vi.advanceTimersByTimeAsync(100);
  frame();
  expect(timeline.visibleWindow).toBe(slice);
});

test('zoom input counts for 1 s', async () => {
  const {host, frame, timeline} = await startFollowing();
  pressW();
  await vi.advanceTimersByTimeAsync(1_100);
  frame();
  expect(host.following).toBe(true);

  timeline.setVisibleWindow(timeline.visibleWindow.scale(0.5, 0.5));
  frame();
  expect(host.following).toBe(false);
});

test('W with Ctrl or in a text field is no zoom input', async () => {
  const {host, frame, timeline, model} = await startFollowing();
  pressW({ctrlKey: true});
  timeline.setVisibleWindow(timeline.visibleWindow.scale(0.5, 0.5));
  frame();
  expect(host.following).toBe(false);

  model().toggleFollow();
  frame();
  const input = document.createElement('input');
  document.body.append(input);
  pressW({bubbles: true}, input);
  input.remove();
  timeline.setVisibleWindow(timeline.visibleWindow.scale(0.5, 0.5));
  frame();
  expect(host.following).toBe(false);
});

test('follows at the new width after a zoom while paused', async () => {
  const {host, frame, timeline, model} = await startFollowing();
  const followed = timeline.visibleWindow;
  model().togglePause();
  pressW();
  timeline.setVisibleWindow(followed.scale(0.5, 0.5));
  frame();

  model().togglePause();
  await vi.advanceTimersByTimeAsync(100);
  frame();

  expect(host.following).toBe(true);
  expect(timeline.visibleWindow.duration).toBeCloseTo(5e9, -1);
  expect(timeline.visibleWindow.end.gte(followed.end.toTime())).toBe(true);
});

test('stops following after a pan while paused', async () => {
  const {host, frame, timeline, model} = await startFollowing();
  model().togglePause();
  const panned = timeline.visibleWindow.translate(-3e9);
  timeline.setVisibleWindow(panned);
  frame();

  model().togglePause();
  await vi.advanceTimersByTimeAsync(100);
  frame();

  expect(host.following).toBe(false);
  expect(timeline.visibleWindow).toBe(panned);
});

test('loads the targets for the target menu', async () => {
  const {host} = await start();

  expect(host.targets.map((t) => t.id)).toEqual([TARGET.id, OTHER_TARGET.id]);
});

test('reopens the trace once when the stream restarts', async () => {
  server.respond(data(5), data(1, 's2'));
  const {host, error} = await start();
  await vi.advanceTimersByTimeAsync(60_000);

  expect(host.reopen.mock.calls).toEqual([[TARGET.id, false]]);
  expect(server.streamRequests.length).toBe(2);
  expect(error()).toBe('The live stream restarted');
});

test('reopens once when the seed clock does not match the stream', async () => {
  server.respond({...data(1), start_epoch_ns: '123'});
  const {host} = await start();
  await vi.advanceTimersByTimeAsync(60_000);

  expect(host.reopen.mock.calls).toEqual([[TARGET.id, true]]);
  expect(server.streamRequests.length).toBe(1);
});

test('reports a seed clock that does not match the stream again', async () => {
  server.respond({...data(1), start_epoch_ns: '123'});
  const {host, error} = await start({epochRetry: true});
  await vi.advanceTimersByTimeAsync(60_000);

  expect(host.reopen).not.toHaveBeenCalled();
  expect(server.streamRequests.length).toBe(1);
  expect(error()).toBe("The seed's clock doesn't match the stream");
});

test('stops polling and leaves the HUD when disposed', async () => {
  const {session, host} = await start();

  session[Symbol.dispose]();
  await vi.advanceTimersByTimeAsync(60_000);

  expect(server.signals[0]!.aborted).toBe(true);
  expect(server.streamRequests.length).toBe(1);
  expect(host.model).toBeUndefined();
});
