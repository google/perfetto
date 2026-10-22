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

// The minimap shows one row per slice track. A cell's load is the time its
// track spent in slices that start in the cell, relative to the cell's width:
//
//   track 1  ░░▓▓██░░    load = min(1, Σ dur / cell width)
//   track 2  ██░░░░▓▓
//
// The minimap has 100 cells of a power-of-2 width (calculateResolution()),
// so cells reach BUCKET_NS for traces over ~107 s. From then on, sums kept per
// bucket build the rows, so that updates cost the same for any history.
// Shorter traces query their few slices.
//
// The core loads the minimap once, so the provider updates the rows that it
// returned in place as the trace grows, see refresh().

import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {calculateResolution} from '../../base/resolution';
import {Time, type duration, type time} from '../../base/time';
import type {MinimapContentProvider, MinimapRow} from '../../public/minimap';
import {LONG, NUM, type QueryResult} from '../../trace_processor/query_result';
import {SLICE_TABLE} from './store';

const BUCKET_NS = 1n << 30n;

/** Runs the SQL of the minimap. */
export interface MinimapEngine {
  query(sql: string): Promise<QueryResult>;
}

/**
 * The busy time of each slice track: the summed durations of its completed
 * slices, by start bucket of BUCKET_NS.
 */
export class BusyTime {
  // Bumped whenever the sums change.
  version = 0;

  // Busy ns by bucket, by track id in the tables.
  private readonly tracks = new Map<number, Map<number, number>>();

  // Replaces the sums with those of the completed slices in SLICE_TABLE.
  async load(engine: MinimapEngine): Promise<void> {
    const result = await engine.query(`
      SELECT track_id AS trackId, ts / ${BUCKET_NS} AS bucket, SUM(dur) AS busy
      FROM ${SLICE_TABLE}
      WHERE dur > 0
      GROUP BY trackId, bucket
    `);
    this.tracks.clear();
    this.version++;
    const spec = {trackId: NUM, bucket: LONG, busy: NUM};
    for (const it = result.iter(spec); it.valid(); it.next()) {
      this.addToBucket(it.trackId, Number(it.bucket), it.busy);
    }
  }

  // Adds a slice that completed.
  add(trackId: number, ts: bigint, dur: bigint): void {
    if (dur <= 0n) return;
    this.addToBucket(trackId, Number(ts / BUCKET_NS), Number(dur));
  }

  // Forgets the slices that start before the bucket of `cutoff`.
  prune(cutoff: bigint): void {
    const first = Number(cutoff / BUCKET_NS);
    for (const [trackId, buckets] of this.tracks) {
      for (const bucket of buckets.keys()) {
        if (bucket < first) {
          buckets.delete(bucket);
          this.version++;
        }
      }
      if (buckets.size === 0) this.tracks.delete(trackId);
    }
  }

  // The rows of [start, end] in cells of `resolution`, a multiple of
  // BUCKET_NS: one per track with slices there, by track id. Cells are
  // computed in Number, which is exact up to 2^53 ns, about 104 days.
  rows(start: bigint, end: bigint, resolution: duration): MinimapRow[] {
    const startNs = Number(start);
    const endNs = Number(end);
    const cellNs = Number(resolution);
    const bucketNs = Number(BUCKET_NS);
    const rows: MinimapRow[] = [];
    const trackIds = [...this.tracks.keys()].sort((a, b) => a - b);
    for (const trackId of trackIds) {
      const busyByCell = new Map<number, number>();
      for (const [bucket, busy] of this.tracks.get(trackId) ?? []) {
        const ts = bucket * bucketNs;
        if (ts < startNs || ts > endNs) continue;
        const cell = Math.floor((ts - startNs) / cellNs);
        busyByCell.set(cell, (busyByCell.get(cell) ?? 0) + busy);
      }
      if (busyByCell.size === 0) continue;
      rows.push(
        [...busyByCell]
          .sort(([a], [b]) => a - b)
          .map(([cell, busy]) => ({
            ts: Time.fromRaw(start + BigInt(cell) * resolution),
            dur: resolution,
            load: Math.min(1, busy / cellNs),
          })),
      );
    }
    return rows;
  }

  private addToBucket(trackId: number, bucket: number, busy: number) {
    let buckets = this.tracks.get(trackId);
    if (buckets === undefined) {
      buckets = new Map();
      this.tracks.set(trackId, buckets);
    }
    buckets.set(bucket, (buckets.get(bucket) ?? 0) + busy);
    this.version++;
  }
}

/** Shows how busy each live slice track is, see the top of this file. */
export class LiveMinimapProvider implements MinimapContentProvider {
  // Wins over the core's providers, which find no data in a seed trace.
  readonly priority = 10;

  // The rows of every getData(), which the core keeps: updates change them in
  // place.
  private readonly rows: MinimapRow[] = [];
  // The bounds and busy time that the rows show. Unset until getData(): the
  // core holds no rows of ours before.
  private shown?: {start: time; end: time; version: number};
  // Numbers the updates: one that ends after a newer one started, e.g. a slow
  // query, drops its rows.
  private lastUpdate = 0;

  constructor(
    private readonly engine: MinimapEngine,
    private readonly busy: BusyTime,
  ) {}

  async getData(
    timeSpan: HighPrecisionTimeSpan,
    resolution: duration,
  ): Promise<MinimapRow[]> {
    const {start, end} = timeSpan.toTimeSpan();
    await this.update(start, end, resolution);
    return this.rows;
  }

  // Updates the rows for the trace bounds [start, end], in cells as the core's
  // load picks them, unless nothing changed. Returns whether it updated them.
  async refresh(start: time, end: time): Promise<boolean> {
    const shown = this.shown;
    if (shown === undefined) return false;
    if (
      start === shown.start &&
      end === shown.end &&
      this.busy.version === shown.version
    ) {
      return false;
    }
    const timeSpan = HighPrecisionTimeSpan.fromTime(start, end);
    const resolution = calculateResolution(timeSpan, 100);
    if (!resolution.ok) return false;
    return this.update(start, end, resolution.value);
  }

  // Replaces the rows with those of [start, end] in cells of `resolution`,
  // unless a newer update started meanwhile. Returns whether it did.
  private async update(
    start: time,
    end: time,
    resolution: duration,
  ): Promise<boolean> {
    const id = ++this.lastUpdate;
    const version = this.busy.version;
    let rows: MinimapRow[] = [];
    if (resolution >= BUCKET_NS) {
      rows = this.busy.rows(start, end, resolution);
    } else if (resolution > 0n) {
      rows = await queryRows(this.engine, start, end, resolution);
    }
    if (id !== this.lastUpdate) return false;
    this.rows.splice(0, this.rows.length, ...rows);
    this.shown = {start, end, version};
    return true;
  }
}

// The rows of a short trace, from its slices.
async function queryRows(
  engine: MinimapEngine,
  start: bigint,
  end: bigint,
  resolution: duration,
): Promise<MinimapRow[]> {
  const result = await engine.query(`
    SELECT
      track_id AS trackId,
      (ts - ${start}) / ${resolution} AS cell,
      MIN(1.0, SUM(dur) / ${Number(resolution)}.0) AS load
    FROM ${SLICE_TABLE}
    WHERE dur > 0 AND ts BETWEEN ${start} AND ${end}
    GROUP BY trackId, cell
    ORDER BY trackId, cell
  `);
  const rows = new Map<number, MinimapRow>();
  const spec = {trackId: NUM, cell: LONG, load: NUM};
  for (const it = result.iter(spec); it.valid(); it.next()) {
    let row = rows.get(it.trackId);
    if (row === undefined) {
      row = [];
      rows.set(it.trackId, row);
    }
    row.push({
      ts: Time.fromRaw(start + it.cell * resolution),
      dur: resolution,
      load: it.load,
    });
  }
  return [...rows.values()];
}
