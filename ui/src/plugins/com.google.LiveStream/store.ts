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

// Trace processor tables holding the streamed data.
//
//   seed trace ──────importSeed()──┐
//   stream response ──ingest()─────┴─► __live_stream_slice   (+ _slice_arg)
//                                      __live_stream_counter
//                                      __live_stream_track   (names)
//                                            │ WHERE track_id = X
//                                            ▼
//                                      SliceTrack / CounterTrack
//
// Hours of rows fit: the store keeps no JS state per row, only the slices
// that responses may send again (recent and active slices, errors). Expired
// rows are deleted in SQL. The seed's slice args aren't copied: they stay in
// the trace's args table.
//
// Full responses are merged like deltas: completed slices and counter points
// that the server no longer holds stay, up to the client retention.

import type {Result} from '../../base/result';
import {sqliteString} from '../../base/string_utils';
import {Time} from '../../base/time';
import {
  LONG,
  NUM,
  type QueryResult,
  STR,
} from '../../trace_processor/query_result';
import type {Slice, StreamResponse} from './wire';

/** Slices of all slice tracks. */
export const SLICE_TABLE = '__live_stream_slice';
/** Args of streamed SLICE_TABLE rows; see sliceArgs() for seed rows. */
export const SLICE_ARG_TABLE = '__live_stream_slice_arg';
/** Points of all counter tracks. */
export const COUNTER_TABLE = '__live_stream_counter';
/** Names of the tracks, by the track_id of the other tables. */
export const TRACK_TABLE = '__live_stream_track';

// The seed's native tracks by the track_id of the other tables, while
// importSeed() copies their rows.
const SEED_TRACK_TABLE = '__live_stream_seed_track';

/**
 * Active slices have no end yet. Trace processor and SliceTrack render
 * slices with dur = -1 as incomplete.
 */
export const ACTIVE_DUR = -1n;

/**
 * Slices longer than this, and active ones, have their own index: a query for
 * the slices overlapping a time range scans the range from LONG_SLICE_NS
 * earlier, plus these. See overlappingSlices().
 */
export const LONG_SLICE_NS = 60_000_000_000n;

// The condition of the index of long and active slices, and of the queries
// that use it.
const LONG_OR_ACTIVE = `dur > ${LONG_SLICE_NS} OR dur = ${ACTIVE_DUR}`;

// Errors are kept regardless of the retention, up to this many.
const MAX_ERRORS = 10_000;

// Completed slices stay known this much longer than full responses may send
// them again.
const RECENT_MARGIN_NS = 10_000_000_000n;

const ERROR_CATEGORY = 'error';

// The description of the seed's data tracks, e.g.
// "com.google.LiveStream:depth=2;track_id=main": the wire track of their rows,
// and the wire depth of their slices.
const SEED_MARKER = /^com\.google\.LiveStream:depth=(\d+);track_id=(.+)$/;

// Prefix of the seed's arg keys: the args of the stream's slices are debug
// annotations.
const SEED_ARG_PREFIX = 'debug.';

// Rows or ids per statement.
const BATCH_SIZE = 500;

// Deeper slices are skipped: a track is as high as its deepest slice.
const MAX_DEPTH = 1000;

/** The subset of Engine used by the store. */
export interface SqlRunner {
  query(sql: string): Promise<QueryResult>;
  tryQuery(sql: string): Promise<Result<unknown>>;
}

/**
 * A failed transaction could not be rolled back, so the tables are in an
 * unknown state. Only a new store recovers.
 */
export class BrokenStoreError extends Error {
  constructor(rollbackError: string) {
    super(`Cannot roll back a failed update: ${rollbackError}`);
    this.name = 'BrokenStoreError';
  }
}

/** Supported track kinds. Tracks of other kinds are ignored. */
export type TrackKind = 'KIND_SLICE' | 'KIND_COUNTER';

/** Identifies a track's rows and their latest change. */
export interface TrackState {
  // Id of the track in the tables.
  readonly sqlId: number;
  readonly kind: TrackKind;
  // Generation of the latest committed change to the track's rows.
  readonly gen: number;
}

/** A time span, in trace time. Both ends are included. */
export interface Span {
  readonly start: bigint;
  readonly end: bigint;
}

/** A slice that completed. */
export interface CompletedSlice {
  // Id of its track in the tables.
  readonly trackId: number;
  readonly ts: bigint;
  readonly dur: bigint;
}

/** The rows that a write changed. */
export interface StoreChanges {
  // Span of the changed rows of each track, by wire track id. Active slices
  // reach Time.MAX.
  readonly spans: ReadonlyMap<string, Span>;
  // The slices that completed, once per slice. Undefined if the write
  // replaced the seed's rows, so that all of them may be new.
  readonly completed?: readonly CompletedSlice[];
}

/** A track of the seed, see LiveStore.importSeed(). */
export interface SeedTrack {
  readonly id: string;
  readonly name: string;
  readonly kind: TrackKind;
  readonly unit: string;
}

interface StoredSlice {
  readonly id: number;
  readonly trackId: string;
  readonly ts: bigint;
  // Time.MAX for active slices.
  readonly end: bigint;
}

interface ActiveSlice extends StoredSlice {
  // Identifies changes, see sliceContent().
  readonly content: string;
}

/** Owns the tables: writes stream responses and prunes expired rows. */
export class LiveStore {
  private readonly tracks = new Map<string, TrackState>();
  private readonly trackNames = new Map<string, string>();
  // Completed slices that a full response may send again, see trimRecent().
  private readonly recent = new Map<string, StoredSlice>();
  private readonly errors = new Map<string, StoredSlice>();
  private active = new Map<string, ActiveSlice>();
  // Errors of the seed, oldest first. Seed rows have no keys.
  private seedErrors: StoredSlice[] = [];
  // The last id of the seed's rows; 0 once a full response replaced them.
  private seedMaxId = 0;
  // The earliest start of each track's rows that may expire.
  private readonly oldestTs = new Map<string, bigint>();
  private nextTrackId = 1;
  private nextSliceId = 1;
  private gen = 0;
  private broken?: BrokenStoreError;

  // `windowNs` is the history that full responses hold.
  constructor(
    private readonly engine: SqlRunner,
    private readonly windowNs: bigint,
  ) {}

  async createTables(): Promise<void> {
    // Seed slices reference their args in the trace's args table: copying
    // them would take most of the import's time and memory.
    await this.engine.query(`
      CREATE TABLE ${SLICE_TABLE}(
        id INTEGER PRIMARY KEY,
        track_id INTEGER NOT NULL,
        ts INTEGER NOT NULL,
        dur INTEGER NOT NULL,
        depth INTEGER NOT NULL,
        name TEXT NOT NULL,
        category TEXT NOT NULL,
        arg_set_id INTEGER
      );
      CREATE INDEX ${SLICE_TABLE}_track ON ${SLICE_TABLE}(track_id, ts);
      CREATE INDEX ${SLICE_TABLE}_long ON ${SLICE_TABLE}(track_id, ts)
        WHERE ${LONG_OR_ACTIVE};
      CREATE TABLE ${SLICE_ARG_TABLE}(
        slice_id INTEGER NOT NULL,
        key TEXT NOT NULL,
        value TEXT NOT NULL
      );
      CREATE INDEX ${SLICE_ARG_TABLE}_slice ON ${SLICE_ARG_TABLE}(slice_id);
      CREATE TABLE ${COUNTER_TABLE}(
        track_id INTEGER NOT NULL,
        ts INTEGER NOT NULL,
        value REAL NOT NULL,
        PRIMARY KEY(track_id, ts)
      );
      CREATE TABLE ${TRACK_TABLE}(
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL
      );
    `);
  }

  // Returns the state of a track seen in the seed or in a response.
  track(id: string): TrackState | undefined {
    return this.tracks.get(id);
  }

  // Copies the rows of the seed trace's marked tracks, at their wire depth,
  // and returns those tracks in server order: none for seeds without
  // markers. Leaves out active slices, which responses hold, and rows that
  // end before `minEnd`, except errors. Call once, before ingest().
  async importSeed(minEnd: bigint): Promise<SeedTrack[]> {
    const result = await this.engine.query(`
      SELECT
        t.id AS nativeId,
        IFNULL(t.name, '') AS name,
        c.id IS NOT NULL AS isCounter,
        IFNULL(c.unit, '') AS unit,
        IFNULL(extract_arg(t.source_arg_set_id, 'description'), '') AS marker
      FROM track t
      LEFT JOIN counter_track c USING (id)
      ORDER BY
        IFNULL(extract_arg(t.source_arg_set_id, 'sibling_order_rank'), 0),
        t.id
    `);

    // A wire slice track has a native track per depth, or more.
    const seedTracks = new Map<string, SeedTrack>();
    const newTracks = new Map<string, TrackState>();
    const nativeRows: string[] = [];
    let nextTrackId = this.nextTrackId;
    const spec = {nativeId: NUM, name: STR, isCounter: NUM, unit: STR};
    for (
      const it = result.iter({...spec, marker: STR});
      it.valid();
      it.next()
    ) {
      const match = SEED_MARKER.exec(it.marker);
      if (match === null) continue;
      const depth = Number(match[1]);
      const id = match[2];
      const kind = it.isCounter ? 'KIND_COUNTER' : 'KIND_SLICE';
      let track = newTracks.get(id);
      if (track === undefined) {
        track = {sqlId: nextTrackId++, kind, gen: 0};
        newTracks.set(id, track);
        seedTracks.set(id, {id, name: it.name, kind, unit: it.unit});
      }
      if (track.kind !== kind || depth >= MAX_DEPTH) continue;
      nativeRows.push(`(${it.nativeId}, ${track.sqlId}, ${depth})`);
    }
    if (seedTracks.size === 0) return [];

    const trackRows = [...seedTracks.values()].map(
      (t) => `(${newTracks.get(t.id)?.sqlId}, ${sqlText(t.name)})`,
    );
    const kept =
      `s.dur >= 0 AND (s.ts + s.dur >= ${minEnd} ` +
      `OR s.category = ${sqlText(ERROR_CATEGORY)})`;

    // Slice ids are the seed's, shifted from 0 to 1: no other row exists yet.
    await this.transact([
      `CREATE TABLE ${SEED_TRACK_TABLE}(
        native_id INTEGER PRIMARY KEY,
        track_id INTEGER NOT NULL,
        depth INTEGER NOT NULL
      )`,
      ...inserts(SEED_TRACK_TABLE, 'native_id, track_id, depth', nativeRows),
      `INSERT INTO ${SLICE_TABLE}(${SLICE_COLUMNS}, arg_set_id)
        SELECT
          s.id + 1, m.track_id, s.ts, s.dur, m.depth,
          IFNULL(s.name, ''), IFNULL(s.category, ''), s.arg_set_id
        FROM slice s
        JOIN ${SEED_TRACK_TABLE} m ON m.native_id = s.track_id
        WHERE ${kept}`,
      `INSERT OR IGNORE INTO ${COUNTER_TABLE}(track_id, ts, value)
        SELECT m.track_id, c.ts, c.value
        FROM counter c
        JOIN ${SEED_TRACK_TABLE} m ON m.native_id = c.track_id
        WHERE c.ts >= ${minEnd}`,
      ...inserts(TRACK_TABLE, 'id, name', trackRows, 'OR REPLACE'),
      `DROP TABLE ${SEED_TRACK_TABLE}`,
    ]);

    for (const [id, track] of newTracks) this.tracks.set(id, track);
    for (const t of seedTracks.values()) this.trackNames.set(t.id, t.name);
    this.nextTrackId = nextTrackId;
    const wireIds = new Map<number, string>();
    for (const [id, track] of newTracks) wireIds.set(track.sqlId, id);
    await this.readSeedState(wireIds);
    return [...seedTracks.values()];
  }

  // Reads what later writes need to know about the seed's rows: errors,
  // oldest rows and last id.
  private async readSeedState(wireIds: ReadonlyMap<number, string>) {
    const errors = await this.engine.query(`
      SELECT id, track_id AS trackId, ts, ts + dur AS end
      FROM ${SLICE_TABLE}
      WHERE category = ${sqlText(ERROR_CATEGORY)}
      ORDER BY end
    `);
    const errorSpec = {id: NUM, trackId: NUM, ts: LONG, end: LONG};
    for (const it = errors.iter(errorSpec); it.valid(); it.next()) {
      const trackId = wireIds.get(it.trackId) ?? '';
      this.seedErrors.push({id: it.id, trackId, ts: it.ts, end: it.end});
    }

    const oldest = await this.engine.query(`
      SELECT track_id AS trackId, MIN(ts) AS ts FROM (
        SELECT track_id, MIN(ts) AS ts FROM ${SLICE_TABLE} GROUP BY track_id
        UNION ALL
        SELECT track_id, MIN(ts) AS ts FROM ${COUNTER_TABLE} GROUP BY track_id
      )
      GROUP BY track_id
    `);
    for (
      const it = oldest.iter({trackId: NUM, ts: LONG});
      it.valid();
      it.next()
    ) {
      const id = wireIds.get(it.trackId);
      if (id !== undefined) this.oldestTs.set(id, it.ts);
    }

    const last = await this.engine.query(
      `SELECT IFNULL(MAX(id), 0) AS id FROM ${SLICE_TABLE}`,
    );
    this.seedMaxId = last.firstRow({id: NUM}).id;
    this.nextSliceId = this.seedMaxId + 1;
  }

  // Applies a stream response in one transaction. On failure the tables and
  // the store are unchanged and the error is rethrown.
  async ingest(resp: StreamResponse): Promise<StoreChanges> {
    const batch = new Batch();

    // Tracks seen for the first time, and new names. Unknown kinds are
    // ignored.
    const newTracks = new Map<string, TrackState>();
    const names = new Map<string, string>();
    let nextTrackId = this.nextTrackId;
    for (const t of resp.tracks) {
      if (t.kind === 'KIND_UNSPECIFIED' || t.id === '') continue;
      let track = this.tracks.get(t.id) ?? newTracks.get(t.id);
      if (track === undefined) {
        track = {sqlId: nextTrackId++, kind: t.kind, gen: 0};
        newTracks.set(t.id, track);
      }
      if ((names.get(t.id) ?? this.trackNames.get(t.id)) !== t.name) {
        names.set(t.id, t.name);
        batch.nameTrack(track, t.name);
      }
    }
    const findTrack = (id: string) => this.tracks.get(id) ?? newTracks.get(id);
    const sliceTrack = (s: Slice) => {
      const track = findTrack(s.track_id);
      const valid = s.key !== '' && s.depth >= 0 && s.depth < MAX_DEPTH;
      return valid && track?.kind === 'KIND_SLICE' ? track : undefined;
    };
    let nextSliceId = this.nextSliceId;

    // A full response holds the seed's latest rows again, with keys.
    const replaceSeed = resp.full && this.seedMaxId > 0;
    if (replaceSeed) {
      batch.deleteSeedRows(this.seedMaxId, resp.playhead_ns - this.windowNs);
      for (const [id, track] of this.tracks) {
        if (track.kind === 'KIND_SLICE') batch.touch(id, Time.MIN, Time.MAX);
      }
    }

    // Writes completed slices, replacing their active rows. The server may
    // send a completed slice again, e.g. at another depth after dropping its
    // state, or in a full response.
    const completed = new Map<string, StoredSlice>();
    const completedErrors = new Set<string>();
    const newlyCompleted: CompletedSlice[] = [];
    for (const s of resp.completed_slices) {
      const track = sliceTrack(s);
      if (track === undefined || completed.has(s.key)) continue;
      const dur = s.dur_ns < 0n ? 0n : s.dur_ns;
      const prev = this.active.get(s.key) ?? this.completedSlice(s.key);
      if (prev !== undefined) batch.deleteSlice(prev);
      const id = prev?.id ?? nextSliceId++;
      batch.insertSlice(id, s, track, dur);
      const end = s.ts_ns + dur;
      completed.set(s.key, {id, trackId: s.track_id, ts: s.ts_ns, end});
      if (s.category === ERROR_CATEGORY) completedErrors.add(s.key);
      if (prev === undefined || prev.end === Time.MAX) {
        newlyCompleted.push({trackId: track.sqlId, ts: s.ts_ns, dur});
      }
    }

    // The response holds all active slices: (re)write the changed ones and
    // delete those that are gone without completing. Completed slices don't
    // become active again.
    const active = new Map<string, ActiveSlice>();
    for (const s of resp.active_slices) {
      const track = sliceTrack(s);
      if (track === undefined || active.has(s.key)) continue;
      if (completed.has(s.key) || this.completedSlice(s.key)) continue;
      const content = sliceContent(s);
      const prev = this.active.get(s.key);
      if (prev?.content === content) {
        active.set(s.key, prev);
        continue;
      }
      if (prev !== undefined) batch.deleteSlice(prev);
      const id = prev?.id ?? nextSliceId++;
      batch.insertSlice(id, s, track, ACTIVE_DUR);
      const trackId = s.track_id;
      active.set(s.key, {id, trackId, ts: s.ts_ns, end: Time.MAX, content});
    }
    for (const [key, prev] of this.active) {
      if (!active.has(key) && !completed.has(key)) batch.deleteSlice(prev);
    }

    // Points already stored are ignored.
    for (const p of resp.counter_points) {
      const track = findTrack(p.track_id);
      if (track?.kind !== 'KIND_COUNTER') continue;
      batch.insertCounter(p.track_id, track, p.ts_ns, p.value);
    }

    await this.transact(batch.statements());

    // Committed: advance the JS state.
    for (const [id, track] of newTracks) this.tracks.set(id, track);
    for (const [id, name] of names) this.trackNames.set(id, name);
    if (replaceSeed) {
      this.seedErrors = [];
      this.seedMaxId = 0;
    }
    for (const [key, slice] of completed) {
      const isError = completedErrors.has(key);
      (isError ? this.recent : this.errors).delete(key);
      (isError ? this.errors : this.recent).set(key, slice);
    }
    this.active = active;
    for (const [id, span] of batch.spans) {
      const oldest = this.oldestTs.get(id);
      if (oldest === undefined || span.start < oldest) {
        this.oldestTs.set(id, span.start);
      }
    }
    this.nextTrackId = nextTrackId;
    this.nextSliceId = nextSliceId;
    this.bumpGen(batch.spans.keys());
    return {
      spans: batch.spans,
      completed: replaceSeed ? undefined : newlyCompleted,
    };
  }

  // Deletes the rows that fell out of the retention, and the oldest errors
  // beyond MAX_ERRORS.
  async prune(playhead: bigint, retentionNs: bigint): Promise<StoreChanges> {
    const cutoff = playhead - retentionNs;
    const batch = new Batch();

    // Only tracks with rows before the cutoff: an index range scan each.
    const pruned: string[] = [];
    for (const [id, track] of this.tracks) {
      const oldest = this.oldestTs.get(id);
      if (oldest === undefined || oldest >= cutoff) continue;
      batch.deleteExpired(id, track, oldest, cutoff);
      pruned.push(id);
    }

    // The oldest errors, the seed's first.
    const excess = this.seedErrors.length + this.errors.size - MAX_ERRORS;
    const seedExpired = this.seedErrors.slice(0, Math.max(0, excess));
    const streamExpired =
      excess > seedExpired.length
        ? [...this.errors]
            .sort(([, a], [, b]) => Number(a.end - b.end) || a.id - b.id)
            .slice(0, excess - seedExpired.length)
        : [];
    for (const error of seedExpired) batch.deleteSlice(error);
    for (const [, error] of streamExpired) batch.deleteSlice(error);

    await this.transact(batch.statements());

    for (const id of pruned) this.oldestTs.set(id, cutoff);
    this.seedErrors = this.seedErrors.slice(seedExpired.length);
    for (const [key] of streamExpired) this.errors.delete(key);
    this.trimRecent(playhead);
    this.bumpGen(batch.spans.keys());
    return {spans: batch.spans, completed: []};
  }

  // Forgets the completed slices that full responses no longer hold: those
  // that ended before their window, which starts at the server's playhead,
  // ahead of this one.
  private trimRecent(playhead: bigint) {
    const minEnd = playhead - this.windowNs - RECENT_MARGIN_NS;
    for (const [key, slice] of this.recent) {
      if (slice.end < minEnd) this.recent.delete(key);
    }
  }

  private completedSlice(key: string): StoredSlice | undefined {
    return this.recent.get(key) ?? this.errors.get(key);
  }

  // Runs `statements` in one transaction, rolling back on failure. Throws
  // BrokenStoreError if the rollback fails, then and for all later calls.
  //
  // Queries that others queue while the transaction runs, e.g. track data
  // loads, run between a failure and the ROLLBACK: they see partial rows, and
  // their own changes, e.g. new tables, are rolled back too. Validated
  // responses hardly fail, so this is accepted.
  private async transact(statements: readonly string[]): Promise<void> {
    if (this.broken !== undefined) throw this.broken;
    if (statements.length === 0) return;
    try {
      await this.engine.query(['BEGIN', ...statements, 'COMMIT'].join(';\n'));
    } catch (e) {
      const rollback = await this.engine.tryQuery('ROLLBACK');
      if (!rollback.ok) {
        this.broken = new BrokenStoreError(rollback.error);
        throw this.broken;
      }
      throw e;
    }
  }

  // Records a committed change of the `touched` tracks.
  private bumpGen(touched: Iterable<string>) {
    let gen: number | undefined;
    for (const id of touched) {
      const track = this.tracks.get(id);
      if (track === undefined) continue;
      gen ??= ++this.gen;
      this.tracks.set(id, {...track, gen});
    }
  }
}

/**
 * Selects `columns` of the slices that overlap [start, end] on the tracks
 * matching `tracks`, a condition on track_id. Costs as many rows as the range
 * holds, not as the tracks hold: each OR branch scans an index range.
 *
 * One SELECT rather than a UNION ALL: SQLite has no plan for SliceTrack's
 * join of a compound query to its mipmap ("no query solution").
 */
export function overlappingSlices(
  columns: string,
  tracks: string,
  start: bigint,
  end: bigint,
): string {
  const longStart = start - LONG_SLICE_NS;
  return `
    SELECT ${columns} FROM ${SLICE_TABLE}
    WHERE (
      (${tracks} AND ts >= ${longStart} AND ts <= ${end})
      OR (${tracks} AND (${LONG_OR_ACTIVE}) AND ts < ${longStart})
    )
      AND (ts + dur >= ${start} OR dur = ${ACTIVE_DUR})
  `;
}

/**
 * Selects the args of slice `id` as (key, value), in order: from
 * SLICE_ARG_TABLE for streamed slices, from the trace's args table for seed
 * slices.
 */
export function sliceArgs(id: number): string {
  return `
    SELECT key, value FROM (
      SELECT key, value, rowid AS ord FROM ${SLICE_ARG_TABLE}
      WHERE slice_id = ${id}
      UNION ALL
      SELECT
        substr(a.key, ${SEED_ARG_PREFIX.length + 1}) AS key,
        IFNULL(a.display_value, '') AS value,
        a.id AS ord
      FROM ${SLICE_TABLE} s
      JOIN args a USING (arg_set_id)
      WHERE s.id = ${id} AND a.key GLOB '${SEED_ARG_PREFIX}*'
    )
    ORDER BY ord
  `;
}

// The statements of one transaction, and the spans of the rows they change.
class Batch {
  readonly spans = new Map<string, Span>();
  private readonly deletes: string[] = [];
  private readonly deletedSliceIds: number[] = [];
  private readonly sliceRows: string[] = [];
  private readonly argRows: string[] = [];
  private readonly counterRows: string[] = [];
  private readonly trackRows: string[] = [];

  touch(trackId: string, start: bigint, end: bigint) {
    const span = this.spans.get(trackId);
    this.spans.set(trackId, {
      start: span !== undefined && span.start < start ? span.start : start,
      end: span !== undefined && span.end > end ? span.end : end,
    });
  }

  nameTrack(track: TrackState, name: string) {
    this.trackRows.push(`(${track.sqlId}, ${sqlText(name)})`);
  }

  deleteSlice(slice: StoredSlice) {
    this.deletedSliceIds.push(slice.id);
    this.touch(slice.trackId, slice.ts, slice.end);
  }

  insertSlice(id: number, s: Slice, track: TrackState, dur: bigint) {
    const name = sqlText(s.name);
    const category = sqlText(s.category);
    this.sliceRows.push(
      `(${id}, ${track.sqlId}, ${s.ts_ns}, ${dur}, ${s.depth}, ${name}, ` +
        `${category})`,
    );
    for (const arg of s.args) {
      this.argRows.push(`(${id}, ${sqlText(arg.key)}, ${sqlText(arg.value)})`);
    }
    const end = dur === ACTIVE_DUR ? Time.MAX : s.ts_ns + dur;
    this.touch(s.track_id, s.ts_ns, end);
  }

  insertCounter(trackId: string, track: TrackState, ts: bigint, value: number) {
    this.counterRows.push(`(${track.sqlId}, ${ts}, ${value})`);
    this.touch(trackId, ts, ts);
  }

  // Deletes a track's rows that ended before `cutoff`, except errors.
  // `oldest` is the earliest start of its rows.
  deleteExpired(
    trackId: string,
    track: TrackState,
    oldest: bigint,
    cutoff: bigint,
  ) {
    const id = track.sqlId;
    if (track.kind === 'KIND_COUNTER') {
      this.deletes.push(
        `DELETE FROM ${COUNTER_TABLE} WHERE track_id = ${id} AND ts < ${cutoff}`,
      );
    } else {
      const expired =
        `track_id = ${id} AND ts < ${cutoff} AND dur >= 0 ` +
        `AND ts + dur < ${cutoff} AND category != ${sqlText(ERROR_CATEGORY)}`;
      this.deletes.push(
        `DELETE FROM ${SLICE_ARG_TABLE} WHERE slice_id IN ` +
          `(SELECT id FROM ${SLICE_TABLE} WHERE ${expired})`,
        `DELETE FROM ${SLICE_TABLE} WHERE ${expired}`,
      );
    }
    this.touch(trackId, oldest, cutoff);
  }

  // Deletes the seed's rows that a full response holds again: errors, and
  // slices that end at or after `minEnd`.
  deleteSeedRows(maxId: number, minEnd: bigint) {
    const resent =
      `id <= ${maxId} AND ` +
      `(category = ${sqlText(ERROR_CATEGORY)} OR ts + dur >= ${minEnd})`;
    this.deletes.push(
      `DELETE FROM ${SLICE_ARG_TABLE} WHERE slice_id IN ` +
        `(SELECT id FROM ${SLICE_TABLE} WHERE ${resent})`,
      `DELETE FROM ${SLICE_TABLE} WHERE ${resent}`,
    );
  }

  // Deletes come first, so that slices can be rewritten with the same id.
  statements(): string[] {
    const sliceDeletes = batches(this.deletedSliceIds).flatMap((ids) => [
      `DELETE FROM ${SLICE_TABLE} WHERE id IN (${ids.join(', ')})`,
      `DELETE FROM ${SLICE_ARG_TABLE} WHERE slice_id IN (${ids.join(', ')})`,
    ]);
    return [
      ...this.deletes,
      ...sliceDeletes,
      ...inserts(TRACK_TABLE, 'id, name', this.trackRows, 'OR REPLACE'),
      ...inserts(SLICE_TABLE, SLICE_COLUMNS, this.sliceRows),
      ...inserts(SLICE_ARG_TABLE, 'slice_id, key, value', this.argRows),
      ...inserts(
        COUNTER_TABLE,
        'track_id, ts, value',
        this.counterRows,
        'OR IGNORE',
      ),
    ];
  }
}

const SLICE_COLUMNS = 'id, track_id, ts, dur, depth, name, category';

function inserts(
  table: string,
  columns: string,
  rows: readonly string[],
  conflict?: 'OR REPLACE' | 'OR IGNORE',
): string[] {
  const insert = conflict === undefined ? 'INSERT' : `INSERT ${conflict}`;
  return batches(rows).map(
    (batch) => `${insert} INTO ${table}(${columns}) VALUES ${batch.join(', ')}`,
  );
}

// Identifies changes of an active slice: its `dur` grows with the playhead
// until it completes.
function sliceContent(s: Slice): string {
  return JSON.stringify([
    s.track_id,
    s.ts_ns.toString(),
    s.depth,
    s.name,
    s.category,
    s.args.map((a) => [a.key, a.value]),
  ]);
}

// Quotes a string for SQL. NUL would end the statement in trace processor.
function sqlText(value: string): string {
  return sqliteString(value.replaceAll('\0', '\uFFFD'));
}

function batches<T>(items: readonly T[]): T[][] {
  const result: T[][] = [];
  for (let i = 0; i < items.length; i += BATCH_SIZE) {
    result.push(items.slice(i, i + BATCH_SIZE));
  }
  return result;
}
