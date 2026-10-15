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

// A live server for demos and tests of the plugin: the protocol of
// ../README.md, streaming simulated training jobs. The slices are
// deterministic per seed and target. Dev controls force the states that the
// client shows, e.g. Buffering and Error.
//
// The file has only type imports, so that Node runs it as is (see
// demo_server.ts) and UI tests answer the plugin's requests in process.
//
//   DemoBackend.handle(url)
//     api/targets ─────────► JOBS
//     api/stream ──────────► StreamBuffer.read(): snapshot or delta
//     api/seed, download ──► writeTrace(): Perfetto protobuf
//     demo/set, restart, state: dev controls
//
//   Session, every 250 ms: Feed.advance() ──► StreamBuffer.publish()

import type {z} from 'zod';
import type {STREAM_RESPONSE, TARGETS_RESPONSE} from '../wire';

// JSON bodies, typed by the plugin's schemas.
type StreamJson = z.input<typeof STREAM_RESPONSE>;
type TargetsJson = z.input<typeof TARGETS_RESPONSE>;
type TrackJson = NonNullable<StreamJson['tracks']>[number];
type SliceJson = NonNullable<StreamJson['completed_slices']>[number];
type PointJson = NonNullable<StreamJson['counter_points']>[number];

// Times are ns since ts 0, as numbers: exact for 104 days.
const MS = 1e6;
const SEC = 1e9;

// As in a live server.
const PUBLISH_MS = 250;
const POLL_MS = 10_000;
const SEED_WAIT_MS = 10_000;
const DEFAULT_WINDOW_SEC = 15;
const MIN_WINDOW_SEC = 1;
const MAX_WINDOW_SEC = 120;
const MAX_ERRORS = 10_000;
const MAX_POINTS_PER_TRACK = 20_000;
const MAX_SESSIONS = 8;
const ERROR_CATEGORY = 'error';
const WAITING_FOR_DATA = 'Waiting for the first snapshot';
const PAGE_TITLE = 'Live stream demo';

// Bounds the heap: at most ~70 MB per session.
const MAX_SLICES = 200_000;

/**
 * Headers of all responses of a live server, the UI's files included: the UI
 * needs cross-origin isolation, and talks only to its own origin.
 */
export const SECURITY_HEADERS: Readonly<Record<string, string>> = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'credentialless',
  'X-Content-Type-Options': 'nosniff',
  'Content-Security-Policy':
    "connect-src 'self' blob: data:; frame-ancestors 'self'; " +
    "object-src 'none'; base-uri 'self'",
};

/** The path of the UI page that shows target `id`, see ../README.md. */
export function uiPath(id: string): string {
  return (
    '/index.html?mode=embedded&hideSidebar=true&hideStatusbar=true' +
    '&enablePlugins=com.google.LiveStream' +
    `&com.google.LiveStream:target=${encodeURIComponent(id)}#!/viewer`
  );
}

/** Settings of DemoBackend, which `demo/set` changes. */
export interface DemoOptions {
  // Seeds the simulation: equal seeds, equal slices per target.
  seed: number;
  // Speed of the jobs, e.g. 2 for twice the steps and tasks per second.
  rate: number;
  // Worker tracks of the targets that have workers.
  lanes: number;
  // History that new sessions start with, in seconds.
  backfill: number;
  // History kept, in seconds: completed slices by end, points by ts. Up to
  // MAX_SLICES slices.
  history: number;
  // Most completed slices and points per delta; `more` pages the rest.
  pageSize: number;
  // IANA time zone of wall-clock times, e.g. "UTC".
  tz: string;
  // Holds new data, as a hung target does: the client shows Buffering.
  stall: boolean;
  // The target's error: holds new data while set; the client shows Error.
  error: string;
  // Fails api/seed with this HTTP status; 0 for none.
  seedStatus: number;
}

/** A response of DemoBackend.handle(). */
export interface DemoResponse {
  readonly status: number;
  readonly headers: Readonly<Record<string, string>>;
  readonly body: Uint8Array;
}

/** A target of `api/targets`. */
export interface DemoTarget {
  readonly id: string;
  readonly short_name: string;
  readonly description: string;
  readonly link: string;
  readonly address: string;
  readonly task_count: number;
  readonly task_index: number;
}

interface Track {
  readonly id: string;
  readonly name: string;
  readonly kind: 'KIND_SLICE' | 'KIND_COUNTER';
  readonly unit: string;
}

interface Arg {
  readonly key: string;
  readonly value: string;
}

interface Slice {
  readonly key: string;
  readonly trackId: string;
  readonly ts: number;
  // The whole duration, also while running.
  readonly dur: number;
  readonly depth: number;
  readonly name: string;
  readonly category: string;
  readonly args: readonly Arg[];
}

interface Point {
  readonly trackId: string;
  readonly ts: number;
  readonly value: number;
}

const NO_ARGS: readonly Arg[] = [];

function arg(key: string, value: string | number): Arg {
  return {key, value: String(value)};
}

// ---------------------------------------------------------------------------
// The simulated jobs.

// Consecutive slices that fill their parent, with relative weights.
interface Phase {
  readonly name: string;
  readonly weight: number;
  readonly children?: readonly Phase[];
}

// The repeated step of a main thread, e.g. a training step.
interface StepKind {
  readonly name: string;
  // Mean duration at rate 1.
  readonly ms: number;
  readonly phases: readonly Phase[];
  // A checkpoint follows every this many steps; 0 for none.
  readonly checkpointEvery: number;
  // A counter that converges over the steps, e.g. the loss.
  readonly metric?: Metric;
}

interface Metric {
  readonly id: string;
  readonly name: string;
  readonly from: number;
  readonly to: number;
}

// What a target runs.
interface Profile {
  readonly step: StepKind;
  readonly input: boolean;
  readonly workers: boolean;
}

const TRAINER: Profile = {
  step: {
    name: 'train_step',
    ms: 600,
    checkpointEvery: 100,
    metric: {id: 'loss', name: 'Loss', from: 4, to: 0.6},
    phases: [
      {name: 'input_wait', weight: 0.06},
      {
        name: 'forward',
        weight: 0.32,
        children: [
          {name: 'embed', weight: 0.15},
          {name: 'layers', weight: 0.7},
          {name: 'loss', weight: 0.15},
        ],
      },
      {
        name: 'backward',
        weight: 0.47,
        children: [
          {name: 'layers_grad', weight: 0.75},
          {name: 'all_reduce', weight: 0.25},
        ],
      },
      {name: 'optimizer', weight: 0.15},
    ],
  },
  input: true,
  workers: true,
};

const EVALUATOR: Profile = {
  step: {
    name: 'eval_step',
    ms: 1500,
    checkpointEvery: 0,
    metric: {id: 'accuracy', name: 'Accuracy', from: 0.35, to: 0.92},
    phases: [
      {name: 'input_wait', weight: 0.1},
      {
        name: 'forward',
        weight: 0.7,
        children: [
          {name: 'embed', weight: 0.15},
          {name: 'layers', weight: 0.85},
        ],
      },
      {name: 'metrics', weight: 0.2},
    ],
  },
  input: true,
  workers: false,
};

const DATA_JOB: Profile = {
  step: {
    name: 'schedule_round',
    ms: 1000,
    checkpointEvery: 0,
    phases: [
      {name: 'plan', weight: 0.2},
      {name: 'dispatch', weight: 0.3},
      {name: 'wait', weight: 0.5},
    ],
  },
  input: false,
  workers: true,
};

const CHECKPOINT_PHASES: readonly Phase[] = [
  {name: 'serialize', weight: 0.35},
  {name: 'write', weight: 0.65},
];

const BATCH_PHASES: readonly Phase[] = [
  {name: 'read', weight: 0.5},
  {name: 'decode', weight: 0.4},
  {name: 'shuffle', weight: 0.1},
];

const TASK_PHASES: readonly Phase[] = [
  {name: 'fetch', weight: 0.3},
  {name: 'compute', weight: 0.7},
];

// A target and what it runs.
interface Job {
  readonly target: DemoTarget;
  readonly profile: Profile;
}

const JOBS: readonly Job[] = [
  job('job-1/trainer', 'Training job 1 · trainer', 4, TRAINER),
  job('job-1/evaluator', 'Training job 1 · evaluator', 1, EVALUATOR),
  job(
    'job-2/distributed-data-preprocessing-coordinator',
    'Data job 2 · coordinator, with a long name',
    16,
    DATA_JOB,
  ),
  job('job-2/reader', 'Data job 2 · reader', 8, DATA_JOB),
];

/** The targets of the demo; the first is the default one. */
export const DEMO_TARGETS: readonly DemoTarget[] = JOBS.map((j) => j.target);

function job(
  id: string,
  description: string,
  taskCount: number,
  profile: Profile,
): Job {
  const [jobName, role] = id.split('/');
  const target = {
    id,
    short_name: role,
    description,
    link: `https://example.com/${jobName}`,
    address: taskAddress(id, 0),
    task_count: taskCount,
    task_index: 0,
  };
  return {target, profile};
}

// E.g. "trainer-3.job-1.example:9000".
function taskAddress(id: string, task: number): string {
  const [jobName, role] = id.split('/');
  return `${role}-${task}.${jobName}.example:9000`;
}

// The job of target `spec`: an id, or "<id>/<task index>" as in
// ../target_spec.ts; the default one if empty.
function findJob(spec: string): Job | undefined {
  if (spec === '') return JOBS[0];
  const exact = JOBS.find((j) => j.target.id === spec);
  if (exact !== undefined) return exact;

  const slash = spec.lastIndexOf('/');
  const base = JOBS.find((j) => j.target.id === spec.slice(0, slash));
  const index = spec.slice(slash + 1);
  if (base === undefined || !/^\d+$/.test(index)) return undefined;
  const task = Number(index);
  if (task >= base.target.task_count) return undefined;
  if (task === base.target.task_index) return base;
  const id = `${base.target.id}/${task}`;
  const address = taskAddress(base.target.id, task);
  return {
    target: {...base.target, id, address, task_index: task},
    profile: base.profile,
  };
}

// A seeded PRNG (mulberry32): equal keys, equal numbers.
class Random {
  private state: number;

  constructor(key: string) {
    // FNV-1a.
    let hash = 0x811c9dc5;
    for (let i = 0; i < key.length; i++) {
      hash = Math.imul(hash ^ key.charCodeAt(i), 0x01000193);
    }
    this.state = hash;
  }

  // Uniform in [0, 1).
  next(): number {
    this.state = (this.state + 0x6d2b79f5) | 0;
    let t = this.state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  }

  between(min: number, max: number): number {
    return min + (max - min) * this.next();
  }

  chance(p: number): boolean {
    return this.next() < p;
  }
}

// Slices and points that a generator adds at once, e.g. a step.
interface Group {
  // The earliest start.
  readonly ts: number;
  readonly slices: readonly Slice[];
  readonly points: readonly Point[];
}

type Groups = Generator<Group, never>;

// A pause between slices of `ms` at rate 1.
function gap(random: Random, rate: () => number, maxMs: number): number {
  return Math.round((random.between(1, maxMs) * MS) / rate());
}

// Adds `phases` as consecutive slices that fill [ts, end) at `depth`, with
// their children below. Weights vary by ±20% each time.
function addPhases(
  out: Slice[],
  parent: Slice,
  phases: readonly Phase[],
  random: Random,
): void {
  const weights = phases.map((p) => p.weight * random.between(0.8, 1.2));
  const total = weights.reduce((sum, w) => sum + w, 0);
  const end = parent.ts + parent.dur;
  let ts = parent.ts;
  let sum = 0;
  phases.forEach((phase, i) => {
    sum += weights[i];
    const stop =
      i === phases.length - 1
        ? end
        : parent.ts + Math.round((parent.dur * sum) / total);
    const slice: Slice = {
      key: `${parent.key}/${i}`,
      trackId: parent.trackId,
      ts,
      dur: stop - ts,
      depth: parent.depth + 1,
      name: phase.name,
      category: 'phase',
      args: NO_ARGS,
    };
    out.push(slice);
    if (phase.children) addPhases(out, slice, phase.children, random);
    ts = stop;
  });
}

// An instant on the errors track.
function errorSlice(key: string, ts: number, name: string, args: Arg[]): Slice {
  const category = ERROR_CATEGORY;
  return {key, trackId: 'errors', ts, dur: 0, depth: 0, name, category, args};
}

// Steps of the main thread, each with its counter points, and a checkpoint
// every `checkpointEvery` steps.
function* mainThread(
  kind: StepKind,
  random: Random,
  rate: () => number,
): Groups {
  let ts = 0;
  for (let step = 1; ; step++) {
    // ±10%; one in 30 is slow, e.g. by a straggler.
    const slow = random.chance(1 / 30) ? random.between(1.5, 2.5) : 1;
    const dur = Math.round(
      (kind.ms * MS * random.between(0.9, 1.1) * slow) / rate(),
    );
    const end = ts + dur;
    const key = `main/${step}`;
    const slices: Slice[] = [];
    const points: Point[] = [
      {trackId: 'step', ts: end, value: step},
      {trackId: 'step_time', ts: end, value: dur / SEC},
    ];
    const args = [arg('Step', step)];
    const metric = kind.metric;
    if (metric !== undefined && random.chance(1 / 500)) {
      const message = arg('Message', 'Skipped the batch');
      const name = `${metric.name} is NaN`;
      slices.push(errorSlice(`${key}/error`, end, name, [args[0], message]));
    } else if (metric !== undefined) {
      const range = metric.from - metric.to;
      const value =
        metric.to +
        range * Math.exp(-step / 1500) +
        range * 0.02 * (random.next() - 0.5);
      points.push({trackId: metric.id, ts: end, value});
      args.push(arg(metric.name, value.toFixed(4)));
    }
    const stepSlice: Slice = {
      key,
      trackId: 'main',
      ts,
      dur,
      depth: 0,
      name: kind.name,
      category: 'step',
      args,
    };
    slices.push(stepSlice);
    addPhases(slices, stepSlice, kind.phases, random);
    yield {ts, slices, points};
    ts = end + gap(random, rate, 10);

    if (kind.checkpointEvery > 0 && step % kind.checkpointEvery === 0) {
      const checkpoint: Slice = {
        key: `${key}/checkpoint`,
        trackId: 'main',
        ts,
        dur: Math.round((random.between(2, 4) * SEC) / rate()),
        depth: 0,
        name: 'save_checkpoint',
        category: 'checkpoint',
        args: [arg('Step', step), arg('Path', `/checkpoints/step-${step}`)],
      };
      const ckptSlices = [checkpoint];
      addPhases(ckptSlices, checkpoint, CHECKPOINT_PHASES, random);
      yield {ts, slices: ckptSlices, points: []};
      ts += checkpoint.dur + gap(random, rate, 10);
    }
  }
}

// Batches of the input pipeline, each with the size of the queue after it.
function* inputPipeline(
  kind: StepKind,
  random: Random,
  rate: () => number,
): Groups {
  let ts = 0;
  let queue = 4;
  for (let batch = 1; ; batch++) {
    const slice: Slice = {
      key: `input/${batch}`,
      trackId: 'input',
      ts,
      dur: Math.round((kind.ms * MS * random.between(0.5, 0.9)) / rate()),
      depth: 0,
      name: 'prefetch_batch',
      category: 'task',
      args: [arg('Batch', batch)],
    };
    const slices = [slice];
    addPhases(slices, slice, BATCH_PHASES, random);
    queue = Math.min(8, Math.max(0, queue + (random.chance(0.5) ? 1 : -1)));
    const end = ts + slice.dur;
    yield {
      ts,
      slices,
      points: [{trackId: 'input_queue', ts: end, value: queue}],
    };
    ts = end + gap(random, rate, 5);
  }
}

// Tasks of worker `lane` from `ts`. One in 150 fails, with a copy on the
// errors track.
function* worker(
  keyPrefix: string,
  lane: number,
  startTs: number,
  random: Random,
  rate: () => number,
): Groups {
  let ts = startTs + gap(random, rate, 50);
  for (let task = 1; ; task++) {
    const key = `${keyPrefix}/${task}`;
    const dur = Math.round((random.between(200, 900) * MS) / rate());
    const failed = random.chance(1 / 150);
    const slices: Slice[] = [];
    const args = [arg('Task', task)];
    if (failed) {
      args.push(arg('Error', 'Deadline exceeded'));
      const message = arg('Message', 'Deadline exceeded; retrying');
      const name = `Worker ${lane}: task failed`;
      slices.push(
        errorSlice(`${key}/error`, ts + dur, name, [args[0], message]),
      );
    }
    const slice: Slice = {
      key,
      trackId: `worker/${lane}`,
      ts,
      dur,
      depth: 0,
      name: 'process_shard',
      category: failed ? ERROR_CATEGORY : 'task',
      args,
    };
    slices.push(slice);
    addPhases(slices, slice, TASK_PHASES, random);
    yield {ts, slices, points: []};
    ts += dur + gap(random, rate, 50);
  }
}

function counterTrack(id: string, name: string, unit: string): Track {
  return {id, name, kind: 'KIND_COUNTER', unit};
}

function sliceTrack(id: string, name: string): Track {
  return {id, name, kind: 'KIND_SLICE', unit: ''};
}

// A generator and the start of the last group it added.
interface Lane {
  readonly groups: Groups;
  ts: number;
}

function lane(groups: Groups): Lane {
  return {groups, ts: -Infinity};
}

/** A change of the stream: what the feed holds at a playhead. */
interface Update {
  readonly playhead: number;
  readonly tracks: readonly Track[];
  readonly completed: readonly Slice[];
  readonly active: readonly Slice[];
  readonly points: readonly Point[];
}

// The simulated job of a target: generators of slices and points, run up to
// the playhead.
class Feed {
  private readonly profile: Profile;
  private readonly options: DemoOptions;
  private readonly seedKey: string;
  private readonly rate: () => number;
  private readonly tracks: Track[];
  private readonly lanes: Lane[] = [];
  // By lane index; undefined while beyond `lanes`.
  private readonly workers: Array<Lane | undefined> = [];
  private readonly workerStarts: number[] = [];
  // Generated, but not yet completed or reached by the playhead.
  private slices: Slice[] = [];
  private points: Point[] = [];

  constructor(profile: Profile, seedKey: string, options: DemoOptions) {
    this.profile = profile;
    this.options = options;
    this.seedKey = seedKey;
    this.rate = () => this.options.rate;

    const step = profile.step;
    const metric = step.metric;
    this.tracks = [
      counterTrack('step', 'Step', ''),
      counterTrack('step_time', 'Step time', 's'),
      ...(metric === undefined
        ? []
        : [counterTrack(metric.id, metric.name, '')]),
      sliceTrack('errors', 'Errors'),
      sliceTrack('main', 'Main thread'),
    ];
    this.lanes.push(lane(mainThread(step, this.random('main'), this.rate)));
    if (profile.input) {
      this.tracks.push(
        sliceTrack('input', 'Input pipeline'),
        counterTrack('input_queue', 'Input queue', 'batches'),
      );
      const random = this.random('input');
      this.lanes.push(lane(inputPipeline(step, random, this.rate)));
    }
    this.updateWorkers(0);
  }

  // Runs the generators up to `playhead`.
  advance(playhead: number): Update {
    this.updateWorkers(playhead);
    for (const l of [...this.lanes, ...this.workers]) {
      while (l !== undefined && l.ts <= playhead) {
        const group = l.groups.next().value;
        l.ts = group.ts;
        this.slices.push(...group.slices);
        this.points.push(...group.points);
      }
    }

    const completed: Slice[] = [];
    const active: Slice[] = [];
    const later: Slice[] = [];
    for (const s of this.slices) {
      if (s.ts + s.dur <= playhead) {
        completed.push(s);
      } else {
        if (s.ts <= playhead) active.push(s);
        later.push(s);
      }
    }
    this.slices = later;

    const points: Point[] = [];
    const laterPoints: Point[] = [];
    for (const p of this.points) {
      (p.ts <= playhead ? points : laterPoints).push(p);
    }
    this.points = laterPoints;
    points.sort((a, b) => a.ts - b.ts);

    completed.sort((a, b) => a.ts - b.ts || a.depth - b.depth);
    active.sort((a, b) => a.ts - b.ts || a.depth - b.depth);
    const tracks = [...this.tracks];
    return {playhead, tracks, completed, active, points};
  }

  // Starts and stops worker lanes to match `lanes`. New lanes start at `ts`
  // and keep their tracks once stopped; running tasks still complete.
  private updateWorkers(ts: number): void {
    if (!this.profile.workers) return;
    const lanes = this.options.lanes;
    for (let i = 0; i < Math.max(lanes, this.workers.length); i++) {
      if (i >= lanes) {
        this.workers[i] = undefined;
        continue;
      }
      if (this.workers[i] !== undefined) continue;
      if (i >= this.workers.length) {
        this.tracks.push(sliceTrack(`worker/${i}`, `Worker ${i}`));
      }
      // Keys and numbers differ per start.
      const start = this.workerStarts[i] ?? 0;
      this.workerStarts[i] = start + 1;
      const prefix = start === 0 ? `worker/${i}` : `worker/${i}.${start}`;
      const random = this.random(prefix);
      this.workers[i] = lane(worker(prefix, i, ts, random, this.rate));
    }
  }

  private random(name: string): Random {
    return new Random(`${this.seedKey}/${name}`);
  }
}

// ---------------------------------------------------------------------------
// The stream state, as a live server holds it.

// A change, by the seq of the publish that added it.
interface Entry<T> {
  readonly seq: number;
  readonly item: T;
}

interface Zone {
  readonly offsetMin: number;
  readonly abbr: string;
}

// What a seed or download trace holds.
interface TraceData {
  readonly targetId: string;
  readonly startEpochNs: bigint;
  readonly tzOffsetMin: number;
  readonly tracks: readonly Track[];
  readonly completed: readonly Slice[];
  readonly active: readonly Slice[];
  readonly points: readonly Point[];
}

// The stream of a session: seq-numbered changes within the retention, for
// snapshots, paged deltas and traces.
class StreamBuffer {
  readonly streamId = randomId();
  seq = 0;
  private readonly targetId: string;
  private readonly startEpochNs: bigint;
  private readonly options: DemoOptions;
  private playhead = 0;
  private zone: Zone;
  private tracks: readonly Track[] = [];
  private active: readonly Slice[] = [];
  private hasData = false;
  private currentError = '';
  // Bumped by errors before the first data, which have no seq.
  private errorVersion = 0;
  // The last seq of an evicted change: deltas need a later base.
  private evictedSeq = 0;
  private disposed = false;
  // Completed slices, by seq. Errors apart: they're kept longer.
  private history: Array<Entry<Slice>> = [];
  private errors: Array<Entry<Slice>> = [];
  private readonly points = new Map<string, Array<Entry<Point>>>();
  private readonly waiters = new Set<() => void>();

  constructor(
    targetId: string,
    startEpochNs: bigint,
    zone: Zone,
    options: DemoOptions,
  ) {
    this.targetId = targetId;
    this.startEpochNs = startEpochNs;
    this.zone = zone;
    this.options = options;
  }

  publish(update: Update, zone: Zone): void {
    const seq = ++this.seq;
    for (const item of update.completed) {
      const store =
        item.category === ERROR_CATEGORY ? this.errors : this.history;
      store.push({seq, item});
    }
    for (const item of update.points) {
      let points = this.points.get(item.trackId);
      if (points === undefined) {
        points = [];
        this.points.set(item.trackId, points);
      }
      points.push({seq, item});
    }
    this.playhead = update.playhead;
    this.tracks = update.tracks;
    this.active = update.active;
    this.zone = zone;
    this.currentError = '';
    this.hasData = true;
    this.evict();
    this.wake();
  }

  // The error of responses; empty while healthy. Before the first data, it
  // also says that none came yet, as a live server does, e.g. "Waiting for
  // the first snapshot: <error>".
  get lastError(): string {
    const error = this.currentError;
    if (this.hasData || error === '') return error;
    return `${WAITING_FOR_DATA}: ${error}`;
  }

  setError(error: string): void {
    if (error === this.currentError) return;
    this.currentError = error;
    if (this.hasData) {
      this.seq++;
    } else {
      this.errorVersion++;
    }
    this.wake();
  }

  // Waits, up to POLL_MS, for changes after `sinceSeq` of stream `streamId`.
  // Then returns a delta from there or, if it can't, a snapshot of the last
  // `windowSec`.
  async read(
    sinceSeq: number,
    streamId: string,
    windowSec: number,
    signal?: AbortSignal,
  ): Promise<StreamJson> {
    // Only a seq seen of this stream is a base for deltas.
    const valid = streamId === this.streamId && sinceSeq <= this.seq;
    const base = valid ? sinceSeq : 0;
    const errorVersion = this.errorVersion;
    await this.wait(
      () =>
        this.seq > base ||
        this.errorVersion !== errorVersion ||
        (!this.hasData && this.currentError !== ''),
      POLL_MS,
      signal,
    );
    // Deltas must not miss evicted changes.
    if (base > 0 && base >= this.evictedSeq) return this.delta(base);
    return this.snapshot(windowSec);
  }

  // Waits, up to `timeoutMs`, for the first data or an error.
  waitForData(timeoutMs: number, signal?: AbortSignal): Promise<void> {
    const ready = () => this.hasData || this.currentError !== '';
    return this.wait(ready, timeoutMs, signal);
  }

  // The data of a seed (no running slices) or of a download. A seed keeps
  // the last `historySec` up to its latest data, as a live server does: the
  // completed slices that end in it, all errors, and the points in it with
  // each track's previous one, e.g. of points at 1, 2 and 15 s, a window
  // from 10 s keeps 2 and 15 s.
  traceData(withActive: boolean, historySec = Infinity): TraceData {
    const latest = (entries: ReadonlyArray<Entry<Slice | Point>>) =>
      entries.reduce((max, e) => Math.max(max, endOf(e.item)), -Infinity);
    const pointLists = [...this.points.values()];
    const end = Math.max(
      latest(this.history),
      latest(this.errors),
      ...pointLists.map(latest),
    );
    const from = end - historySec * SEC;
    const points: Point[] = [];
    for (const list of pointLists) {
      const previous = list.reduce(
        (max, e) => (e.item.ts < from ? Math.max(max, e.item.ts) : max),
        -Infinity,
      );
      for (const e of list) if (e.item.ts >= previous) points.push(e.item);
    }
    const kept = this.history.filter((e) => endOf(e.item) >= from);
    return {
      targetId: this.targetId,
      startEpochNs: this.startEpochNs,
      tzOffsetMin: this.zone.offsetMin,
      tracks: this.tracks,
      completed: [...kept, ...this.errors].map((e) => e.item),
      active: withActive ? this.active : [],
      points,
    };
  }

  describe() {
    let points = 0;
    for (const p of this.points.values()) points += p.length;
    return {
      target: this.targetId,
      stream_id: this.streamId,
      seq: this.seq,
      playhead_ns: this.playhead,
      last_error: this.lastError,
      completed_slices: this.history.length + this.errors.length,
      active_slices: this.active.length,
      counter_points: points,
    };
  }

  // Returns pending reads and waits at once.
  dispose(): void {
    this.disposed = true;
    this.wake();
  }

  private snapshot(windowSec: number): StreamJson {
    const minEnd =
      this.playhead - Math.min(windowSec, this.options.history) * SEC;
    const completed = this.history
      .filter((e) => e.item.ts + e.item.dur >= minEnd)
      .concat(this.errors);
    const points: Array<Entry<Point>> = [];
    for (const p of this.points.values()) {
      points.push(...p.filter((e) => e.item.ts >= minEnd));
    }
    return this.response(this.seq, true, completed, points);
  }

  private delta(base: number): StreamJson {
    const end = this.deltaEnd(base);
    const completed = [
      ...between(this.history, base, end),
      ...between(this.errors, base, end),
    ];
    const points: Array<Entry<Point>> = [];
    for (const p of this.points.values()) points.push(...between(p, base, end));
    return this.response(end, false, completed, points);
  }

  // The seq that a delta from `base` reaches: whole publishes with at most
  // `pageSize` entries, but at least one publish.
  private deltaEnd(base: number): number {
    const counts = new Map<number, number>();
    const count = (entries: ReadonlyArray<Entry<unknown>>) => {
      for (let i = firstAfter(entries, base); i < entries.length; i++) {
        const seq = entries[i].seq;
        counts.set(seq, (counts.get(seq) ?? 0) + 1);
      }
    };
    count(this.history);
    count(this.errors);
    for (const p of this.points.values()) count(p);

    let total = 0;
    for (const [seq, n] of [...counts].sort((a, b) => a[0] - b[0])) {
      if (total > 0 && total + n > this.options.pageSize) return seq - 1;
      total += n;
    }
    return this.seq;
  }

  private response(
    seq: number,
    full: boolean,
    completed: ReadonlyArray<Entry<Slice>>,
    points: ReadonlyArray<Entry<Point>>,
  ): StreamJson {
    return {
      stream_id: this.streamId,
      seq: String(seq),
      full,
      target: this.targetId,
      playhead_ns: String(this.playhead),
      start_epoch_ns: String(this.startEpochNs),
      tz_offset_min: this.zone.offsetMin,
      tz_abbr: this.zone.abbr,
      tracks: this.tracks.map(trackJson),
      completed_slices: completed.map((e) => sliceJson(e.item, e.item.dur)),
      active_slices: this.active.map((s) => sliceJson(s, this.playhead - s.ts)),
      counter_points: points.map((e) => pointJson(e.item)),
      last_error: this.lastError,
      more: seq < this.seq,
    };
  }

  // Drops completed slices that end, and points from, before the history,
  // and the oldest beyond the caps. Each store is in seq order, so evicted
  // changes precede kept ones.
  private evict(): void {
    const minEnd = this.playhead - this.options.history * SEC;
    const evict = <T>(entries: Array<Entry<T>>, count: number) => {
      if (count <= 0) return;
      this.evictedSeq = Math.max(this.evictedSeq, entries[count - 1].seq);
      entries.splice(0, count);
    };
    const old = (entries: ReadonlyArray<Entry<Slice | Point>>) => {
      let n = 0;
      while (n < entries.length && endOf(entries[n].item) < minEnd) n++;
      return n;
    };
    const history = this.history;
    evict(history, Math.max(old(history), history.length - MAX_SLICES));
    evict(this.errors, this.errors.length - MAX_ERRORS);
    for (const p of this.points.values()) {
      evict(p, Math.max(old(p), p.length - MAX_POINTS_PER_TRACK));
    }
  }

  // Resolves once `ready()`, after `timeoutMs`, or once disposed or aborted.
  private wait(
    ready: () => boolean,
    timeoutMs: number,
    signal?: AbortSignal,
  ): Promise<void> {
    if (ready() || this.disposed || signal?.aborted) return Promise.resolve();
    return new Promise((resolve) => {
      const done = () => {
        clearTimeout(timer);
        this.waiters.delete(check);
        signal?.removeEventListener('abort', done);
        resolve();
      };
      const check = () => {
        if (ready() || this.disposed) done();
      };
      const timer = setTimeout(done, timeoutMs);
      this.waiters.add(check);
      signal?.addEventListener('abort', done);
    });
  }

  private wake(): void {
    for (const check of [...this.waiters]) check();
  }
}

function endOf(item: Slice | Point): number {
  return 'dur' in item ? item.ts + item.dur : item.ts;
}

// The index of the first entry after `seq`.
function firstAfter(entries: ReadonlyArray<Entry<unknown>>, seq: number) {
  let lo = 0;
  let hi = entries.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (entries[mid].seq <= seq) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

// The entries with a seq in (from, to].
function between<T>(
  entries: ReadonlyArray<Entry<T>>,
  from: number,
  to: number,
) {
  const out: Array<Entry<T>> = [];
  for (let i = firstAfter(entries, from); i < entries.length; i++) {
    if (entries[i].seq > to) break;
    out.push(entries[i]);
  }
  return out;
}

function trackJson(t: Track): TrackJson {
  return {id: t.id, name: t.name, kind: t.kind, unit: t.unit};
}

function sliceJson(s: Slice, dur: number): SliceJson {
  return {
    key: s.key,
    track_id: s.trackId,
    ts_ns: String(s.ts),
    dur_ns: String(dur),
    depth: s.depth,
    name: s.name,
    category: s.category,
    args: [...s.args],
  };
}

function pointJson(p: Point): PointJson {
  return {track_id: p.trackId, ts_ns: String(p.ts), value: p.value};
}

function randomId(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(8));
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

// Time zone formats by zone: creating them is slow.
const zoneFormats = new Map<
  string,
  [Intl.DateTimeFormat, Intl.DateTimeFormat]
>();

// The UTC offset and abbreviation of time zone `tz` at `epochMs`, e.g. -420
// and "PDT". Throws RangeError for unknown zones.
function zoneAt(tz: string, epochMs: number): Zone {
  let formats = zoneFormats.get(tz);
  if (formats === undefined) {
    const format = (timeZoneName: 'longOffset' | 'short') =>
      new Intl.DateTimeFormat('en-US', {timeZone: tz, timeZoneName});
    formats = [format('longOffset'), format('short')];
    zoneFormats.set(tz, formats);
  }
  const name = (format: Intl.DateTimeFormat) =>
    format.formatToParts(epochMs).find((p) => p.type === 'timeZoneName')
      ?.value ?? '';
  // E.g. "GMT-07:00"; "GMT" for UTC.
  const offset = /([+-])(\d\d):(\d\d)$/.exec(name(formats[0]));
  const sign = offset?.[1] === '-' ? -1 : 1;
  const offsetMin =
    offset === null ? 0 : sign * (Number(offset[2]) * 60 + Number(offset[3]));
  return {offsetMin, abbr: name(formats[1])};
}

// ---------------------------------------------------------------------------
// Sessions and HTTP routes.

// The stream of a target: its feed, published every PUBLISH_MS.
class Session {
  readonly job: Job;
  readonly buffer: StreamBuffer;
  private readonly feed: Feed;
  private readonly options: DemoOptions;
  private readonly startMs: number;
  private readonly timer: ReturnType<typeof setInterval>;

  constructor(job: Job, options: DemoOptions) {
    const nowMs = Date.now();
    this.job = job;
    this.options = options;
    this.startMs = nowMs - Math.round(options.backfill * 1000);
    const seedKey = `${options.seed}/${job.target.id}`;
    this.feed = new Feed(job.profile, seedKey, options);
    this.buffer = new StreamBuffer(
      job.target.id,
      BigInt(this.startMs) * 1_000_000n,
      zoneAt(options.tz, nowMs),
      options,
    );
    this.tick();
    this.timer = setInterval(() => this.tick(), PUBLISH_MS);
  }

  // Publishes the feed up to now, unless a dev control holds the stream.
  tick(): void {
    if (this.options.error !== '') {
      this.buffer.setError(this.options.error);
    } else if (!this.options.stall) {
      const nowMs = Date.now();
      const update = this.feed.advance((nowMs - this.startMs) * MS);
      this.buffer.publish(update, zoneAt(this.options.tz, nowMs));
    }
  }

  dispose(): void {
    clearInterval(this.timer);
    this.buffer.dispose();
  }
}

const DEFAULT_OPTIONS: DemoOptions = {
  seed: 1,
  rate: 1,
  lanes: 4,
  backfill: 120,
  history: 3600,
  pageSize: 20_000,
  tz: Intl.DateTimeFormat().resolvedOptions().timeZone,
  stall: false,
  error: '',
  seedStatus: 0,
};

// Numeric options by `demo/set` key. The caps bound the heap and the first
// publish: up to ~1k slices per second, and ~600k for the backfill.
const NUMERIC_OPTIONS = new Map<string, NumericOption>([
  ['seed', {name: 'seed', min: 0, max: 2 ** 32 - 1, integer: true}],
  ['rate', {name: 'rate', min: 0.01, max: 10, integer: false}],
  ['lanes', {name: 'lanes', min: 0, max: 16, integer: true}],
  ['backfill', {name: 'backfill', min: 0, max: 600, integer: false}],
  ['history', {name: 'history', min: 10, max: 6 * 3600, integer: false}],
  ['page_size', {name: 'pageSize', min: 1, max: 1e6, integer: true}],
  ['seed_status', {name: 'seedStatus', min: 0, max: 599, integer: true}],
]);

interface NumericOption {
  readonly name:
    | 'seed'
    | 'rate'
    | 'lanes'
    | 'backfill'
    | 'history'
    | 'pageSize'
    | 'seedStatus';
  readonly min: number;
  readonly max: number;
  readonly integer: boolean;
}

// Sets an option from `demo/set?<key>=<value>`; returns an error message.
function setOption(options: DemoOptions, key: string, value: string) {
  const bad = `bad ${key}: ${echo(value)}`;
  switch (key) {
    case 'stall':
      if (!/^(0|1|false|true)$/.test(value)) return bad;
      options.stall = value === '1' || value === 'true';
      return undefined;
    case 'error':
      options.error = value;
      return undefined;
    case 'tz':
      try {
        zoneAt(value, 0);
      } catch {
        return bad;
      }
      options.tz = value;
      return undefined;
    default:
      break;
  }
  const spec = NUMERIC_OPTIONS.get(key);
  if (spec === undefined) return `unknown setting ${echo(key)}`;
  const n = value.trim() === '' ? NaN : Number(value);
  const valid =
    n >= spec.min && n <= spec.max && (!spec.integer || Number.isInteger(n));
  // Seeds fail with errors only.
  if (!valid || (key === 'seed_status' && n > 0 && n < 400)) return bad;
  options[spec.name] = n;
  return undefined;
}

/**
 * The demo server's state and routes, without HTTP: see the file comment.
 * Sessions publish on timers until dispose().
 */
export class DemoBackend {
  readonly options: DemoOptions;
  private readonly sessions = new Map<string, Session>();
  private disposed = false;

  constructor(options: Partial<DemoOptions> = {}) {
    this.options = {...DEFAULT_OPTIONS, ...options};
    zoneAt(this.options.tz, 0);
  }

  /**
   * Answers a request to an `api/` or `demo/` route of the server, e.g.
   * "/api/stream?target=job-1%2Ftrainer"; undefined for other paths.
   * Long-polls end early once `signal` aborts.
   */
  handle(
    url: string | URL,
    signal?: AbortSignal,
  ): Promise<DemoResponse> | undefined {
    const {pathname, searchParams} = new URL(url, 'http://localhost');
    const route = /^\/(api|demo)\/(\w+)$/.exec(pathname);
    if (route === null) return undefined;
    const reply =
      route[1] === 'api'
        ? this.api(route[2], searchParams, signal)
        : Promise.resolve(this.demo(route[2], searchParams));
    return reply.catch((e) => errorReply(500, `demo failed: ${e}`));
  }

  /**
   * Changes options as `demo/set?<key>=<value>&...` does, e.g. with
   * [['stall', '1']]. Returns an error message, with no change, for unknown
   * keys and bad values. `seed`, `backfill` and `tz` apply to new sessions,
   * see `demo/restart`.
   */
  set(entries: Iterable<[string, string]>): string | undefined {
    const options = {...this.options};
    for (const [key, value] of entries) {
      const error = setOption(options, key, value);
      if (error !== undefined) return error;
    }
    Object.assign(this.options, options);
    // At once, rather than at the next publish.
    for (const session of this.sessions.values()) session.tick();
    return undefined;
  }

  /** The URL path of the UI for `?target=<spec>`: the default if unknown. */
  pagePath(spec: string): string {
    return uiPath((findJob(spec) ?? JOBS[0]).target.id);
  }

  /** Stops all sessions; pending requests return at once. */
  dispose(): void {
    this.disposed = true;
    for (const session of this.sessions.values()) session.dispose();
    this.sessions.clear();
  }

  private async api(
    route: string,
    params: URLSearchParams,
    signal?: AbortSignal,
  ): Promise<DemoResponse> {
    if (!['targets', 'stream', 'seed', 'download'].includes(route)) {
      return errorReply(404, `unknown API route ${echo(`/api/${route}`)}`);
    }
    if (this.disposed) return errorReply(503, 'the demo stopped');
    const spec = params.get('target') ?? '';
    const job = findJob(spec);
    if (job === undefined) {
      return errorReply(404, `unknown target ${echo(spec)}`);
    }

    // Also for api/targets: the seed is ready sooner.
    const session = this.session(job);
    switch (route) {
      case 'targets': {
        const targets = [...DEMO_TARGETS];
        const body: TargetsJson = {
          targets,
          selected: job.target,
          page_title: PAGE_TITLE,
        };
        return jsonReply(body);
      }
      case 'stream':
        return this.stream(session, params, signal);
      default:
        return this.trace(session, route === 'download', params, signal);
    }
  }

  private async stream(
    session: Session,
    params: URLSearchParams,
    signal?: AbortSignal,
  ): Promise<DemoResponse> {
    const since = params.get('since_seq') ?? '0';
    if (!/^\d+$/.test(since)) {
      const got = echo(since);
      return errorReply(
        400,
        `since_seq must be a non-negative integer, got ${got}`,
      );
    }
    const windowParam = params.get('window');
    let windowSec = DEFAULT_WINDOW_SEC;
    if (windowParam !== null) {
      windowSec = windowParam.trim() === '' ? NaN : Number(windowParam);
      if (!Number.isFinite(windowSec)) {
        const got = echo(windowParam);
        return errorReply(
          400,
          `window must be a number of seconds, got ${got}`,
        );
      }
      windowSec = Math.min(MAX_WINDOW_SEC, Math.max(MIN_WINDOW_SEC, windowSec));
    }
    const streamId = params.get('stream_id') ?? '';
    const sinceSeq = Number(since);
    const body = await session.buffer.read(
      sinceSeq,
      streamId,
      windowSec,
      signal,
    );
    return jsonReply(body);
  }

  // A seed: the latest `history_sec` (default all) of history, with track
  // markers, without running slices. Or a download: all data, for other
  // tools.
  private async trace(
    session: Session,
    download: boolean,
    params: URLSearchParams,
    signal?: AbortSignal,
  ): Promise<DemoResponse> {
    const history = download ? null : params.get('history_sec');
    if (history !== null && !/^0*[1-9]\d{0,14}$/.test(history)) {
      const got = echo(history);
      return errorReply(
        400,
        `history_sec must be a positive integer, got ${got}`,
      );
    }
    const historySec = history === null ? Infinity : Number(history);
    const status = this.options.seedStatus;
    if (!download && status !== 0) {
      return errorReply(status, `api/seed fails with ${status}, see demo/set`);
    }
    const buffer = session.buffer;
    await buffer.waitForData(SEED_WAIT_MS, signal);
    // As a live server does: 503 while no data came yet, and 502 with the
    // error once getting it fails.
    if (buffer.seq === 0) {
      const error = buffer.lastError;
      if (error !== '') return errorReply(502, error);
      return errorReply(503, `no data from ${session.job.target.id} yet`);
    }
    const headers: Record<string, string> = {
      'Content-Type': 'application/octet-stream',
      'Cache-Control': 'no-store',
      'X-Live-Stream-Id': buffer.streamId,
      'X-Live-Seq': String(buffer.seq),
    };
    if (download) {
      headers['Content-Disposition'] =
        'attachment; filename="live_trace.pftrace"';
    }
    const body = writeTrace(buffer.traceData(download, historySec), !download);
    return {status: 200, headers, body};
  }

  private demo(route: string, params: URLSearchParams): DemoResponse {
    switch (route) {
      case 'set': {
        const error = this.set(params);
        return error === undefined
          ? jsonReply(this.options)
          : errorReply(400, error);
      }
      case 'restart': {
        // The next responses have a new stream_id: clients reopen the trace.
        const spec = params.get('target');
        const job = spec === null ? undefined : findJob(spec);
        if (spec !== null && job === undefined) {
          return errorReply(404, `unknown target ${echo(spec)}`);
        }
        const sessions = [...this.sessions.values()].filter(
          (s) => job === undefined || s.job.target.id === job.target.id,
        );
        for (const session of sessions) {
          session.dispose();
          this.sessions.delete(session.job.target.id);
          this.session(session.job);
        }
        return jsonReply({restarted: sessions.map((s) => s.job.target.id)});
      }
      case 'state': {
        const sessions = [...this.sessions.values()];
        return jsonReply({
          options: this.options,
          sessions: sessions.map((s) => s.buffer.describe()),
        });
      }
      default:
        return errorReply(404, `unknown demo route ${echo(`/demo/${route}`)}`);
    }
  }

  // The session of `job`, created if needed. Keeps the MAX_SESSIONS most
  // recently used.
  private session(job: Job): Session {
    const id = job.target.id;
    let session = this.sessions.get(id);
    this.sessions.delete(id);
    session ??= new Session(job, this.options);
    this.sessions.set(id, session);
    for (const [oldId, old] of this.sessions) {
      if (this.sessions.size <= MAX_SESSIONS) break;
      old.dispose();
      this.sessions.delete(oldId);
    }
    return session;
  }
}

const TEXT_ENCODER = new TextEncoder();

function jsonReply(body: unknown, status = 200): DemoResponse {
  return {
    status,
    headers: {'Content-Type': 'application/json', 'Cache-Control': 'no-store'},
    body: TEXT_ENCODER.encode(JSON.stringify(body)),
  };
}

function errorReply(status: number, error: string): DemoResponse {
  return jsonReply({error}, status);
}

// A request value for messages: quoted, escaped and bounded.
function echo(value: string): string {
  const more = value.length > 100 ? '...' : '';
  return `${JSON.stringify(value.slice(0, 100))}${more}`;
}

// ---------------------------------------------------------------------------
// Perfetto traces, see protos/perfetto/trace/.

// Field numbers.
const TRACE_PACKET = 1;
const PACKET_CLOCK_SNAPSHOT = 6;
const PACKET_TIMESTAMP = 8;
const PACKET_SEQUENCE_ID = 10;
const PACKET_TRACK_EVENT = 11;
const PACKET_SYSTEM_INFO = 45;
const PACKET_TRACK_DESCRIPTOR = 60;
const SNAPSHOT_CLOCKS = 1;
const CLOCK_ID = 1;
const CLOCK_TIMESTAMP = 2;
const SYSTEM_INFO_TZ_OFFSET_MIN = 7;
const TRACK_UUID = 1;
const TRACK_NAME = 2;
const TRACK_PARENT_UUID = 5;
const TRACK_COUNTER = 8;
const TRACK_CHILD_ORDERING = 11;
const TRACK_SIBLING_ORDER_RANK = 12;
const TRACK_DESCRIPTION = 14;
const TRACK_MERGE_BEHAVIOR = 15;
const TRACK_MERGE_KEY = 16;
const COUNTER_UNIT_NAME = 6;
const EVENT_DEBUG_ANNOTATIONS = 4;
const EVENT_TYPE = 9;
const EVENT_TRACK_UUID = 11;
const EVENT_CATEGORIES = 22;
const EVENT_NAME = 23;
const EVENT_DOUBLE_COUNTER_VALUE = 44;
const ANNOTATION_STRING_VALUE = 6;
const ANNOTATION_NAME = 10;

// Enum values.
const CLOCK_REALTIME = 1;
const CLOCK_BOOTTIME = 6;
const ORDERING_EXPLICIT = 3;
const MERGE_NONE = 2;
const MERGE_BY_KEY = 3;
const TYPE_SLICE_BEGIN = 1;
const TYPE_SLICE_END = 2;
const TYPE_INSTANT = 3;
const TYPE_COUNTER = 4;

// All packets are of one sequence, as track events require.
const SEQUENCE_ID = 1;
const GROUP_UUID = 1;

// Writes protobuf messages, nested ones through a reused child writer.
class ProtoWriter {
  private buf = new Uint8Array(1024);
  private view = new DataView(this.buf.buffer);
  private len = 0;
  private child?: ProtoWriter;

  // An integer field. Negative values take 10 bytes, as in int32 and int64.
  int(field: number, value: number | bigint): void {
    this.varint(field * 8);
    this.varint(value);
  }

  double(field: number, value: number): void {
    this.varint(field * 8 + 1);
    this.reserve(8);
    this.view.setFloat64(this.len, value, true);
    this.len += 8;
  }

  string(field: number, value: string): void {
    this.bytes(field, TEXT_ENCODER.encode(value));
  }

  message(field: number, write: (child: ProtoWriter) => void): void {
    const child = (this.child ??= new ProtoWriter());
    child.len = 0;
    write(child);
    this.bytes(field, child.buf.subarray(0, child.len));
  }

  finish(): Uint8Array {
    return this.buf.slice(0, this.len);
  }

  private bytes(field: number, bytes: Uint8Array): void {
    this.varint(field * 8 + 2);
    this.varint(bytes.length);
    this.reserve(bytes.length);
    this.buf.set(bytes, this.len);
    this.len += bytes.length;
  }

  private varint(value: number | bigint): void {
    this.reserve(10);
    if (typeof value === 'number' && value >= 0) {
      // Exact for safe integers, unlike 32-bit bitwise operators.
      let v = value;
      for (; v >= 0x80; v = Math.floor(v / 0x80)) {
        this.buf[this.len++] = (v % 0x80) | 0x80;
      }
      this.buf[this.len++] = v;
      return;
    }
    let v = BigInt.asUintN(64, BigInt(value));
    for (; v >= 0x80n; v >>= 7n) {
      this.buf[this.len++] = Number(v & 0x7fn) | 0x80;
    }
    this.buf[this.len++] = Number(v);
  }

  private reserve(n: number): void {
    if (this.len + n <= this.buf.length) return;
    const buf = new Uint8Array(Math.max(this.buf.length * 2, this.len + n));
    buf.set(this.buf.subarray(0, this.len));
    this.buf = buf;
    this.view = new DataView(buf.buffer);
  }
}

// A slice with its end: Infinity while running.
interface Placed {
  readonly slice: Slice;
  readonly end: number;
}

// A track of properly nested slices, as a track event track needs: of one
// wire depth with markers, else of all.
interface ChildTrack {
  readonly depth: number;
  readonly slices: Placed[];
  // Ends of the open slices, innermost last.
  readonly open: number[];
}

// Splits the slices of a wire track into child tracks: each slice goes on
// the first child of its depth whose innermost open slice contains it, else
// on a new child.
function layOut(slices: readonly Placed[], markers: boolean): ChildTrack[] {
  const sorted = [...slices].sort(
    (a, b) =>
      a.slice.ts - b.slice.ts ||
      (a.end === b.end ? 0 : a.end > b.end ? -1 : 1) ||
      (a.slice.key < b.slice.key ? -1 : a.slice.key > b.slice.key ? 1 : 0),
  );
  const children: ChildTrack[] = [];
  for (const placed of sorted) {
    const depth = markers ? placed.slice.depth : 0;
    let child = children.find((c) => c.depth === depth && fits(c, placed));
    if (child === undefined) {
      child = {depth, slices: [], open: []};
      children.push(child);
    }
    child.slices.push(placed);
    child.open.push(placed.end);
  }
  return children;
}

// Whether `placed` nests in the innermost slice of `child` still open at its
// start.
function fits(child: ChildTrack, placed: Placed): boolean {
  const open = child.open;
  while (open.length > 0 && open[open.length - 1] <= placed.slice.ts) {
    open.pop();
  }
  return open.length === 0 || placed.end <= open[open.length - 1];
}

// The track description that maps seed tracks to the stream's.
function marker(depth: number, trackId: string): string {
  return `com.google.LiveStream:depth=${depth};track_id=${trackId}`;
}

// Writes `data` as a Perfetto trace: the tracks with data as children of a
// group track, in wire order. With `markers`, for seeds, the descriptions
// map the tracks to wire tracks and depths. Without, each wire track shows
// as one track.
function writeTrace(data: TraceData, markers: boolean): Uint8Array {
  const out = new ProtoWriter();
  const packet = (ts: number | undefined, write: (p: ProtoWriter) => void) => {
    out.message(TRACE_PACKET, (p) => {
      if (ts !== undefined) p.int(PACKET_TIMESTAMP, ts);
      p.int(PACKET_SEQUENCE_ID, SEQUENCE_ID);
      write(p);
    });
  };

  // ts 0 of the trace clock is start_epoch_ns of the wall clock.
  packet(undefined, (p) =>
    p.message(PACKET_CLOCK_SNAPSHOT, (snapshot) => {
      snapshot.message(SNAPSHOT_CLOCKS, (c) => {
        c.int(CLOCK_ID, CLOCK_BOOTTIME);
        c.int(CLOCK_TIMESTAMP, 0);
      });
      snapshot.message(SNAPSHOT_CLOCKS, (c) => {
        c.int(CLOCK_ID, CLOCK_REALTIME);
        c.int(CLOCK_TIMESTAMP, data.startEpochNs);
      });
    }),
  );
  packet(undefined, (p) =>
    p.message(PACKET_SYSTEM_INFO, (info) =>
      info.int(SYSTEM_INFO_TZ_OFFSET_MIN, data.tzOffsetMin),
    ),
  );

  const slices = new Map<string, Placed[]>();
  const place = (slice: Slice, end: number) => {
    const placed = slices.get(slice.trackId) ?? [];
    placed.push({slice, end});
    slices.set(slice.trackId, placed);
  };
  for (const s of data.completed) place(s, s.ts + s.dur);
  for (const s of data.active) place(s, Infinity);
  const points = new Map<string, Point[]>();
  for (const p of data.points) {
    const list = points.get(p.trackId) ?? [];
    list.push(p);
    points.set(p.trackId, list);
  }

  let uuid = GROUP_UUID;
  const descriptors: Array<() => void> = [];
  const events: Array<() => void> = [];
  data.tracks.forEach((track, rank) => {
    const descriptor = (depth: number, write: (d: ProtoWriter) => void) => {
      const trackUuid = ++uuid;
      descriptors.push(() =>
        packet(undefined, (p) =>
          p.message(PACKET_TRACK_DESCRIPTOR, (d) => {
            d.int(TRACK_UUID, trackUuid);
            d.int(TRACK_PARENT_UUID, GROUP_UUID);
            d.string(TRACK_NAME, track.name);
            d.int(TRACK_SIBLING_ORDER_RANK, rank);
            if (markers) d.string(TRACK_DESCRIPTION, marker(depth, track.id));
            write(d);
          }),
        ),
      );
      return trackUuid;
    };

    const trackPoints = points.get(track.id) ?? [];
    if (track.kind === 'KIND_COUNTER' && trackPoints.length > 0) {
      const trackUuid = descriptor(0, (d) =>
        d.message(TRACK_COUNTER, (c) => {
          if (track.unit !== '') c.string(COUNTER_UNIT_NAME, track.unit);
        }),
      );
      events.push(() => {
        for (const point of trackPoints) {
          writeCounter(packet, trackUuid, point);
        }
      });
    }
    if (track.kind !== 'KIND_SLICE') return;
    for (const child of layOut(slices.get(track.id) ?? [], markers)) {
      // Children with one merge key show as one track.
      const key = markers ? marker(child.depth, track.id) : track.id;
      const trackUuid = descriptor(child.depth, (d) => {
        d.int(TRACK_MERGE_BEHAVIOR, MERGE_BY_KEY);
        d.string(TRACK_MERGE_KEY, key);
      });
      events.push(() => writeSlices(packet, trackUuid, child.slices));
    }
  });
  if (descriptors.length === 0) return out.finish();

  packet(undefined, (p) =>
    p.message(PACKET_TRACK_DESCRIPTOR, (d) => {
      d.int(TRACK_UUID, GROUP_UUID);
      d.string(TRACK_NAME, data.targetId);
      d.int(TRACK_CHILD_ORDERING, ORDERING_EXPLICIT);
      d.int(TRACK_MERGE_BEHAVIOR, MERGE_NONE);
    }),
  );
  for (const write of [...descriptors, ...events]) write();
  return out.finish();
}

type PacketWriter = (
  ts: number | undefined,
  write: (p: ProtoWriter) => void,
) => void;

// Writes the slices of a child track, in start order, each start after the
// ends up to it. Running slices get no end.
function writeSlices(
  packet: PacketWriter,
  trackUuid: number,
  slices: readonly Placed[],
): void {
  const open: number[] = [];
  const closeUntil = (ts: number) => {
    let end = open.at(-1);
    while (end !== undefined && end <= ts) {
      open.pop();
      packet(end, (p) =>
        p.message(PACKET_TRACK_EVENT, (e) => {
          e.int(EVENT_TYPE, TYPE_SLICE_END);
          e.int(EVENT_TRACK_UUID, trackUuid);
        }),
      );
      end = open.at(-1);
    }
  };
  for (const {slice, end} of slices) {
    closeUntil(slice.ts);
    const instant = slice.dur === 0;
    packet(slice.ts, (p) =>
      p.message(PACKET_TRACK_EVENT, (e) => {
        e.int(EVENT_TYPE, instant ? TYPE_INSTANT : TYPE_SLICE_BEGIN);
        e.int(EVENT_TRACK_UUID, trackUuid);
        e.string(EVENT_NAME, slice.name);
        if (slice.category !== '') e.string(EVENT_CATEGORIES, slice.category);
        for (const a of slice.args) {
          e.message(EVENT_DEBUG_ANNOTATIONS, (d) => {
            d.string(ANNOTATION_NAME, a.key);
            d.string(ANNOTATION_STRING_VALUE, a.value);
          });
        }
      }),
    );
    if (!instant) open.push(end);
  }
  closeUntil(Number.MAX_VALUE);
}

function writeCounter(packet: PacketWriter, trackUuid: number, point: Point) {
  packet(point.ts, (p) =>
    p.message(PACKET_TRACK_EVENT, (e) => {
      e.int(EVENT_TYPE, TYPE_COUNTER);
      e.int(EVENT_TRACK_UUID, trackUuid);
      e.double(EVENT_DOUBLE_COUNTER_VALUE, point.value);
    }),
  );
}
