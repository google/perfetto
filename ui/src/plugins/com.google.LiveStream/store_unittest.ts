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

import {errResult, okResult, type Result} from '../../base/result';
import {Time} from '../../base/time';
import type {QueryResult} from '../../trace_processor/query_result';
import {
  BrokenStoreError,
  COUNTER_TABLE,
  LiveStore,
  overlappingSlices,
  SLICE_ARG_TABLE,
  SLICE_TABLE,
  type SqlRunner,
  TRACK_TABLE,
} from './store';
import {queryResult} from './test_utils';
import {STREAM_RESPONSE} from './wire';

const SEC = 1_000_000_000n;

// History of full responses.
const WINDOW_NS = 120n * SEC;

const INSERT_SLICES = `INSERT INTO ${SLICE_TABLE}(id, track_id, ts, dur, depth, name, category) VALUES`;
const INSERT_ARGS = `INSERT INTO ${SLICE_ARG_TABLE}(slice_id, key, value) VALUES`;
const INSERT_POINTS = `INSERT OR IGNORE INTO ${COUNTER_TABLE}(track_id, ts, value) VALUES`;
const NAME_TRACKS = `INSERT OR REPLACE INTO ${TRACK_TABLE}(id, name) VALUES`;

// Names the test tracks when the store first sees them.
const NAME_TEST_TRACKS = `${NAME_TRACKS} (1, 'Tasks'), (2, 'Step time')`;

// Deletes slice `ids` and their args.
function deleteSlices(ids: string): string[] {
  return [
    `DELETE FROM ${SLICE_TABLE} WHERE id IN (${ids})`,
    `DELETE FROM ${SLICE_ARG_TABLE} WHERE slice_id IN (${ids})`,
  ];
}

// Deletes the slices that match `condition`, and their args.
function deleteSlicesWhere(condition: string): string[] {
  return [
    `DELETE FROM ${SLICE_ARG_TABLE} WHERE slice_id IN ` +
      `(SELECT id FROM ${SLICE_TABLE} WHERE ${condition})`,
    `DELETE FROM ${SLICE_TABLE} WHERE ${condition}`,
  ];
}

// The completed slices of slice track 1 that ended before `cutoff`.
function expired(cutoff: bigint): string {
  return (
    `track_id = 1 AND ts < ${cutoff} AND dur >= 0 ` +
    `AND ts + dur < ${cutoff} AND category != 'error'`
  );
}

// Records queries, failing the next one when `failNext` is set, and the next
// tryQuery() when `failNextTry` is set. Queries return the rows of `respond`,
// if any.
class FakeEngine implements SqlRunner {
  readonly queries: string[] = [];
  failNext = false;
  failNextTry = false;
  respond: (sql: string) => QueryResult | undefined = () => undefined;

  async query(sql: string): Promise<QueryResult> {
    this.queries.push(sql);
    if (this.failNext) {
      this.failNext = false;
      throw new Error('query failed');
    }
    return this.respond(sql) ?? queryResult([]);
  }

  async tryQuery(sql: string): Promise<Result<unknown>> {
    this.queries.push(sql);
    if (this.failNextTry) {
      this.failNextTry = false;
      return errResult('try query failed');
    }
    return okResult();
  }

  // Returns the statements of the recorded queries, without BEGIN and COMMIT,
  // and forgets the queries.
  takeStatements(): string[] {
    const statements = this.queries
      .flatMap((q) => q.split(';\n'))
      .filter((s) => s !== 'BEGIN' && s !== 'COMMIT');
    this.queries.length = 0;
    return statements;
  }
}

// Slice track "tasks" gets id 1, counter track "steps" id 2.
const TRACKS = [
  {id: 'tasks', name: 'Tasks', kind: 'KIND_SLICE'},
  {id: 'steps', name: 'Step time', kind: 'KIND_COUNTER', unit: 's'},
];

// A stream response with the test tracks, in the server's JSON encoding.
function response(json: object) {
  return STREAM_RESPONSE.parse({seq: '1', tracks: TRACKS, ...json});
}

function slice(key: string, tsNs: bigint, fields: object = {}) {
  return {key, track_id: 'tasks', ts_ns: tsNs.toString(), name: key, ...fields};
}

function error(key: string, tsNs: bigint) {
  return slice(key, tsNs, {dur_ns: '1', category: 'error'});
}

function point(tsNs: bigint, value: number) {
  return {track_id: 'steps', ts_ns: tsNs.toString(), value};
}

// Answers the queries of LiveStore.importSeed() with a seed of the test
// tracks: native track 1 holds "steps", tracks 2 and 3 depths 0 and 1 of
// "tasks". Its slices have ids up to `lastId`, and `errors` are
// [id, ts, end] on "tasks".
function seed(lastId: number, errors: Array<[number, number, number]> = []) {
  const marker = (depth: number, id: string) =>
    `com.google.LiveStream:depth=${depth};track_id=${id}`;
  return (sql: string) => {
    if (sql.includes('FROM track t')) {
      return queryResult(
        ['nativeId', 'name', 'isCounter', 'unit', 'marker'],
        [
          [0, 'Group', 0, '', ''],
          [1, 'Step time', 1, 's', marker(0, 'steps')],
          [2, 'Tasks', 0, '', marker(0, 'tasks')],
          [3, 'Tasks', 0, '', marker(1, 'tasks')],
        ],
      );
    }
    if (sql.includes('ORDER BY end')) {
      return queryResult(
        ['id', 'trackId', 'ts', 'end'],
        errors.map(([id, ts, end]) => [id, 2, ts, end]),
      );
    }
    if (sql.includes('UNION ALL')) {
      return queryResult(
        ['trackId', 'ts'],
        [
          [1, 0],
          [2, 0],
        ],
      );
    }
    if (sql.includes('MAX(id)')) return queryResult(['id'], [[lastId]]);
    return undefined;
  };
}

let engine: FakeEngine;
let store: LiveStore;

beforeEach(() => {
  engine = new FakeEngine();
  store = new LiveStore(engine, WINDOW_NS);
});

test('writes a response in one transaction', async () => {
  const changes = await store.ingest(
    response({
      completed_slices: [slice('done', 10n, {dur_ns: '20', category: 'cat'})],
      active_slices: [slice('run', 100n, {args: [{key: 'k', value: 'v'}]})],
      counter_points: [point(5n, 1.5)],
    }),
  );

  expect(engine.queries).toEqual([
    [
      'BEGIN',
      NAME_TEST_TRACKS,
      `${INSERT_SLICES} (1, 1, 10, 20, 0, 'done', 'cat'), ` +
        `(2, 1, 100, -1, 0, 'run', '')`,
      `${INSERT_ARGS} (2, 'k', 'v')`,
      `${INSERT_POINTS} (2, 5, 1.5)`,
      'COMMIT',
    ].join(';\n'),
  ]);
  expect(changes).toEqual({
    spans: new Map([
      ['tasks', {start: 10n, end: Time.MAX}],
      ['steps', {start: 5n, end: 5n}],
    ]),
    completed: [{trackId: 1, ts: 10n, dur: 20n}],
  });
  expect(store.track('tasks')).toEqual({sqlId: 1, kind: 'KIND_SLICE', gen: 1});
  expect(store.track('steps')).toEqual({
    sqlId: 2,
    kind: 'KIND_COUNTER',
    gen: 1,
  });
});

test('names new tracks without changing rows', async () => {
  const changes = await store.ingest(response({}));

  expect(changes).toEqual({spans: new Map(), completed: []});
  expect(engine.takeStatements()).toEqual([NAME_TEST_TRACKS]);
  expect(store.track('tasks')).toEqual({sqlId: 1, kind: 'KIND_SLICE', gen: 0});

  // Known names aren't written again.
  await store.ingest(response({}));
  expect(engine.queries).toEqual([]);
});

test('renames tracks', async () => {
  await store.ingest(response({}));
  engine.takeStatements();

  const renamed = [{...TRACKS[0], name: 'Renamed'}, TRACKS[1]];
  const changes = await store.ingest(response({tracks: renamed}));

  expect(engine.takeStatements()).toEqual([`${NAME_TRACKS} (1, 'Renamed')`]);
  expect(changes.spans.size).toBe(0);
  expect(store.track('tasks')?.gen).toBe(0);
});

test('rewrites a completed active slice with the same id', async () => {
  await store.ingest(response({active_slices: [slice('a', 100n)]}));
  engine.takeStatements();

  const changes = await store.ingest(
    response({completed_slices: [slice('a', 100n, {dur_ns: '50'})]}),
  );

  expect(engine.takeStatements()).toEqual([
    ...deleteSlices('1'),
    `${INSERT_SLICES} (1, 1, 100, 50, 0, 'a', '')`,
  ]);
  expect(changes).toEqual({
    spans: new Map([['tasks', {start: 100n, end: Time.MAX}]]),
    completed: [{trackId: 1, ts: 100n, dur: 50n}],
  });
});

test('writes active slices only when they change', async () => {
  await store.ingest(response({active_slices: [slice('a', 100n)]}));
  engine.takeStatements();

  // The duration of active slices grows with the playhead.
  const grown = slice('a', 100n, {dur_ns: '900'});
  const changes = await store.ingest(response({active_slices: [grown]}));
  expect(changes.spans.size).toBe(0);
  expect(engine.queries).toEqual([]);
  expect(store.track('tasks')?.gen).toBe(1);

  const renamed = slice('a', 100n, {name: 'renamed'});
  await store.ingest(response({active_slices: [renamed]}));
  expect(engine.takeStatements()).toEqual([
    ...deleteSlices('1'),
    `${INSERT_SLICES} (1, 1, 100, -1, 0, 'renamed', '')`,
  ]);
});

test('deletes active slices that end without completing', async () => {
  await store.ingest(
    response({active_slices: [slice('a', 100n), slice('b', 200n)]}),
  );
  engine.takeStatements();

  await store.ingest(response({active_slices: [slice('b', 200n)]}));

  expect(engine.takeStatements()).toEqual(deleteSlices('1'));
});

test('keeps completed slices and points that full responses omit', async () => {
  await store.ingest(
    response({
      completed_slices: [slice('old', 10n, {dur_ns: '5'})],
      counter_points: [point(10n, 1)],
    }),
  );
  engine.takeStatements();

  await store.ingest(
    response({full: true, completed_slices: [slice('new', 20n)]}),
  );

  expect(engine.takeStatements()).toEqual([
    `${INSERT_SLICES} (2, 1, 20, 0, 0, 'new', '')`,
  ]);
});

test('rewrites repeated completed slices in place', async () => {
  const done = slice('a', 10n, {dur_ns: '5'});
  await store.ingest(
    response({completed_slices: [done], counter_points: [point(10n, 1)]}),
  );
  engine.takeStatements();

  // E.g. in a full response. Stored points are ignored in SQL.
  const changes = await store.ingest(
    response({
      completed_slices: [done],
      active_slices: [slice('a', 10n)],
      counter_points: [point(10n, 3)],
    }),
  );

  expect(engine.takeStatements()).toEqual([
    ...deleteSlices('1'),
    `${INSERT_SLICES} (1, 1, 10, 5, 0, 'a', '')`,
    `${INSERT_POINTS} (2, 10, 3)`,
  ]);
  // Counted once.
  expect(changes.completed).toEqual([]);
});

test('replaces completed slices that change', async () => {
  await store.ingest(
    response({completed_slices: [slice('a', 10n, {dur_ns: '5'})]}),
  );
  engine.takeStatements();

  // E.g. emitted again at another depth.
  await store.ingest(
    response({completed_slices: [slice('a', 10n, {dur_ns: '5', depth: 1})]}),
  );

  expect(engine.takeStatements()).toEqual([
    ...deleteSlices('1'),
    `${INSERT_SLICES} (1, 1, 10, 5, 1, 'a', '')`,
  ]);
});

test('skips invalid rows', async () => {
  const changes = await store.ingest(
    response({
      tracks: [...TRACKS, {id: 'future', kind: 'KIND_HEATMAP'}],
      completed_slices: [
        slice('', 10n),
        slice('negative depth', 10n, {depth: -1}),
        slice('too deep', 10n, {depth: 1000}),
        slice('counter track', 10n, {track_id: 'steps'}),
        slice('unknown track', 10n, {track_id: 'unknown'}),
        slice('unknown kind', 10n, {track_id: 'future'}),
      ],
      counter_points: [
        {track_id: 'tasks', ts_ns: '1', value: 1},
        {track_id: 'unknown', ts_ns: '1', value: 1},
      ],
    }),
  );

  expect(changes.spans.size).toBe(0);
  expect(engine.takeStatements()).toEqual([NAME_TEST_TRACKS]);
  expect(store.track('future')).toBeUndefined();
});

test('clamps negative durations of completed slices', async () => {
  await store.ingest(
    response({completed_slices: [slice('a', 10n, {dur_ns: '-5'})]}),
  );

  expect(engine.takeStatements()).toEqual([
    NAME_TEST_TRACKS,
    `${INSERT_SLICES} (1, 1, 10, 0, 0, 'a', '')`,
  ]);
});

test('quotes strings', async () => {
  const quoted = slice('a', 10n, {
    name: "it's",
    category: 'nul\0char',
    args: [{key: "'", value: ''}],
  });
  const tracks = [{...TRACKS[0], name: "Bob's"}];
  await store.ingest(response({tracks, active_slices: [quoted]}));

  expect(engine.takeStatements()).toEqual([
    `${NAME_TRACKS} (1, 'Bob''s')`,
    `${INSERT_SLICES} (1, 1, 10, -1, 0, 'it''s', 'nul\uFFFDchar')`,
    `${INSERT_ARGS} (1, '''', '')`,
  ]);
});

test('leaves the store unchanged when a transaction fails', async () => {
  const resp = response({active_slices: [slice('a', 10n)]});
  engine.failNext = true;

  await expect(store.ingest(resp)).rejects.toThrow('query failed');
  expect(engine.queries.at(-1)).toBe('ROLLBACK');
  expect(store.track('tasks')).toBeUndefined();
  engine.takeStatements();

  // A retry writes the same rows.
  await store.ingest(resp);
  expect(engine.takeStatements()).toEqual([
    NAME_TEST_TRACKS,
    `${INSERT_SLICES} (1, 1, 10, -1, 0, 'a', '')`,
  ]);
  expect(store.track('tasks')?.gen).toBe(1);
});

test('refuses all work once a rollback fails', async () => {
  const resp = response({active_slices: [slice('a', 10n)]});
  engine.failNext = true;
  engine.failNextTry = true;

  await expect(store.ingest(resp)).rejects.toThrow(BrokenStoreError);
  engine.takeStatements();
  await expect(store.ingest(resp)).rejects.toThrow(BrokenStoreError);
  await expect(store.prune(0n, 60n * SEC)).rejects.toThrow(BrokenStoreError);
  expect(engine.queries).toEqual([]);
});

test('splits large changes into batches', async () => {
  const points = Array.from({length: 1001}, (_, i) => point(BigInt(i), i));
  await store.ingest(response({counter_points: points}));

  const statements = engine.takeStatements().slice(1);
  expect(statements.length).toBe(3);
  expect(statements.every((s) => s.startsWith(INSERT_POINTS))).toBe(true);
});

test('bumps the generation of changed tracks only', async () => {
  const tracks = [...TRACKS, {id: 'other', kind: 'KIND_SLICE'}];
  const other = (name: string) => slice('b', 10n, {track_id: 'other', name});
  await store.ingest(
    response({tracks, active_slices: [slice('a', 10n), other('b')]}),
  );

  await store.ingest(
    response({tracks, active_slices: [slice('a', 10n), other('renamed')]}),
  );

  expect(store.track('tasks')?.gen).toBe(1);
  expect(store.track('other')?.gen).toBe(2);
  expect(store.track('steps')?.gen).toBe(0);
});

test('prunes rows that fall out of the retention in SQL', async () => {
  await store.ingest(
    response({
      completed_slices: [slice('old', 0n, {dur_ns: (10n * SEC).toString()})],
      active_slices: [slice('running', 0n)],
      counter_points: [point(0n, 1), point(100n * SEC, 2)],
    }),
  );
  engine.takeStatements();

  // Keeps 120 s: errors and active slices stay regardless.
  const changes = await store.prune(200n * SEC, 120n * SEC);
  expect(engine.takeStatements()).toEqual([
    ...deleteSlicesWhere(expired(80n * SEC)),
    `DELETE FROM ${COUNTER_TABLE} WHERE track_id = 2 AND ts < 80000000000`,
  ]);
  expect(changes).toEqual({
    spans: new Map([
      ['tasks', {start: 0n, end: 80n * SEC}],
      ['steps', {start: 0n, end: 80n * SEC}],
    ]),
    completed: [],
  });
  expect(store.track('tasks')?.gen).toBe(2);

  // Nothing is older than the cutoff any more.
  await store.prune(200n * SEC, 120n * SEC);
  expect(engine.queries).toEqual([]);

  await store.prune(300n * SEC, 120n * SEC);
  expect(engine.takeStatements()).toEqual([
    ...deleteSlicesWhere(expired(180n * SEC)),
    `DELETE FROM ${COUNTER_TABLE} WHERE track_id = 2 AND ts < 180000000000`,
  ]);
});

test('keeps the newest 10k errors', async () => {
  // The oldest last, as the order of arrival doesn't matter.
  const errors = Array.from({length: 10_002}, (_, i) =>
    error(`error ${i}`, BigInt(10_001 - i)),
  );
  await store.ingest(response({completed_slices: errors}));
  engine.takeStatements();

  await store.prune(0n, 60n * SEC);

  expect(engine.takeStatements()).toEqual(deleteSlices('10002, 10001'));
});

test('forgets completed slices that full responses no longer hold', async () => {
  const old = slice('old', 0n, {dur_ns: SEC.toString()});
  await store.ingest(response({completed_slices: [old]}));
  // Ended before the window of full responses, but within the retention.
  await store.prune(200n * SEC, 3600n * SEC);
  engine.takeStatements();

  await store.ingest(response({completed_slices: [old]}));

  expect(engine.takeStatements()).toEqual([
    `${INSERT_SLICES} (2, 1, 0, 1000000000, 0, 'old', '')`,
  ]);
});

test('imports the marked tracks of the seed', async () => {
  engine.respond = seed(7);

  const tracks = await store.importSeed(-5n * SEC);

  expect(tracks).toEqual([
    {id: 'steps', name: 'Step time', kind: 'KIND_COUNTER', unit: 's'},
    {id: 'tasks', name: 'Tasks', kind: 'KIND_SLICE', unit: ''},
  ]);
  const transaction =
    engine.queries.find((q) => q.startsWith('BEGIN'))?.split(';\n') ?? [];
  expect(transaction).toContain(
    'INSERT INTO __live_stream_seed_track(native_id, track_id, depth) ' +
      'VALUES (1, 1, 0), (2, 2, 0), (3, 2, 1)',
  );
  expect(transaction).toContain(
    `${NAME_TRACKS} (1, 'Step time'), (2, 'Tasks')`,
  );
  // Completed slices that end after the cutoff, or are errors, at the depth
  // of their native track.
  const copy = transaction.find((s) =>
    s.startsWith(`INSERT INTO ${SLICE_TABLE}`),
  );
  expect(copy).toContain('s.id + 1, m.track_id, s.ts, s.dur, m.depth');
  expect(copy).toContain(
    "s.dur >= 0 AND (s.ts + s.dur >= -5000000000 OR s.category = 'error')",
  );
  // Args stay in the trace's args table.
  expect(copy).toContain('s.arg_set_id');
  expect(transaction.some((s) => s.includes(SLICE_ARG_TABLE))).toBe(false);
  expect(transaction.slice(-2)).toEqual([
    'DROP TABLE __live_stream_seed_track',
    'COMMIT',
  ]);
  expect(store.track('tasks')).toEqual({sqlId: 2, kind: 'KIND_SLICE', gen: 0});
  engine.takeStatements();

  // New rows follow the seed's ids. Known names aren't written again.
  await store.ingest(response({active_slices: [slice('a', 10n)]}));
  expect(engine.takeStatements()).toEqual([
    `${INSERT_SLICES} (8, 2, 10, -1, 0, 'a', '')`,
  ]);
});

test('imports nothing from seeds without markers', async () => {
  engine.respond = (sql) =>
    sql.includes('FROM track t')
      ? queryResult(
          ['nativeId', 'name', 'isCounter', 'unit', 'marker'],
          [[0, 'Other', 0, '', 'Some description']],
        )
      : undefined;

  expect(await store.importSeed(0n)).toEqual([]);
  expect(engine.takeStatements().length).toBe(1);
});

test('replaces the seed rows that a full response holds again', async () => {
  engine.respond = seed(7);
  await store.importSeed(0n);
  engine.takeStatements();

  const resp = response({
    full: true,
    playhead_ns: (200n * SEC).toString(),
    completed_slices: [slice('a', 150n * SEC, {dur_ns: '1'})],
  });
  const changes = await store.ingest(resp);

  // Errors, and slices within the window of full responses.
  expect(engine.takeStatements()).toEqual([
    ...deleteSlicesWhere(
      "id <= 7 AND (category = 'error' OR ts + dur >= 80000000000)",
    ),
    `${INSERT_SLICES} (8, 2, 150000000000, 1, 0, 'a', '')`,
  ]);
  expect(changes.spans.get('tasks')).toEqual({start: Time.MIN, end: Time.MAX});
  // All slices may be new.
  expect(changes.completed).toBeUndefined();

  // Once.
  await store.ingest(resp);
  expect(engine.takeStatements()).toEqual([
    ...deleteSlices('8'),
    `${INSERT_SLICES} (8, 2, 150000000000, 1, 0, 'a', '')`,
  ]);
});

test('drops the errors of the seed first', async () => {
  engine.respond = seed(2, [
    [1, 10, 11],
    [2, 20, 21],
  ]);
  await store.importSeed(0n);
  const errors = Array.from({length: 9_999}, (_, i) =>
    error(`error ${i}`, BigInt(i)),
  );
  await store.ingest(response({completed_slices: errors}));
  engine.takeStatements();

  await store.prune(0n, 60n * SEC);

  // Even though stream errors are older.
  expect(engine.takeStatements()).toEqual(deleteSlices('1'));
});

test('selects overlapping slices with index range scans', () => {
  const sql = overlappingSlices('id', 'track_id = 1', 100n * SEC, 200n * SEC);

  // Slices that start in the range or up to a minute earlier...
  expect(sql).toContain(
    '(track_id = 1 AND ts >= 40000000000 AND ts <= 200000000000)',
  );
  // ...or longer or active slices that start earlier...
  expect(sql).toContain(
    'OR (track_id = 1 AND (dur > 60000000000 OR dur = -1) AND ts < 40000000000)',
  );
  // ...and end in the range.
  expect(sql).toContain('AND (ts + dur >= 100000000000 OR dur = -1)');

  // SliceTrack cannot join a compound query to its mipmap.
  expect(sql).not.toContain('UNION');
});
