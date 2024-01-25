// Copyright (C) 2023 The Android Open Source Project
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

import {v4 as uuidv4} from 'uuid';

import {BigintMath} from '../../base/bigint_math';
import {assertFalse} from '../../base/logging';
import {duration, Time, time} from '../../base/time';
import {colorForTid} from '../../common/colorizer';
import {LIMIT, TrackData} from '../../common/track_data';
import {EngineProxy, TimelineFetcher} from '../../common/track_helper';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {PanelSize} from '../../frontend/panel';
import {Track} from '../../public';
import {NUM} from '../../trace_processor/query_result';

export const PROCESS_SUMMARY_TRACK = 'ProcessSummaryTrack';

// TODO(dproy): Consider deduping with CPU summary data.
interface Data extends TrackData {
  bucketSize: duration;
  utilizations: Float64Array;
}

export interface Config {
  pidForColor: number;
  upid: number|null;
  utid: number;
}

const MARGIN_TOP = 5;
const RECT_HEIGHT = 30;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;
const SUMMARY_HEIGHT = TRACK_HEIGHT - MARGIN_TOP;

export class ProcessSummaryTrack implements Track {
  private fetcher = new TimelineFetcher<Data>(this.onBoundsChange.bind(this));
  private engine: EngineProxy;
  private uuid = uuidv4();
  private config: Config;

  constructor(engine: EngineProxy, config: Config) {
    this.engine = engine;
    this.config = config;
  }

  // Returns a valid SQL table name with the given prefix that should be unique
  // for each track.
  private tableName(prefix: string) {
    // Derive table name from, since that is unique for each track.
    // Track ID can be UUID but '-' is not valid for sql table name.
    const idSuffix = this.uuid.split('-').join('_');
    return `${prefix}_${idSuffix}`;
  }

  async onCreate(): Promise<void> {
    await this.engine.query(
      `create virtual table ${this.tableName('window')} using window;`);

    let utids = [this.config.utid];
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    if (this.config.upid) {
      const threadQuery = await this.engine.query(
        `select utid from thread where upid=${this.config.upid}`);
      utids = [];
      for (const it = threadQuery.iter({utid: NUM}); it.valid(); it.next()) {
        utids.push(it.utid);
      }
    }

    const trackQuery = await this.engine.query(
      `select id from thread_track where utid in (${utids.join(',')})`);
    const tracks = [];
    for (const it = trackQuery.iter({id: NUM}); it.valid(); it.next()) {
      tracks.push(it.id);
    }

    const processSliceView = this.tableName('process_slice_view');
    await this.engine.query(
      `create view ${processSliceView} as ` +
        // 0 as cpu is a dummy column to perform span join on.
        `select ts, dur/${utids.length} as dur ` +
        `from slice s ` +
        `where depth = 0 and track_id in ` +
        `(${tracks.join(',')})`);
    await this.engine.query(`create virtual table ${this.tableName('span')}
        using span_join(${processSliceView},
                        ${this.tableName('window')});`);
  }

  async onUpdate(): Promise<void> {
    this.fetcher.requestDataForCurrentTime();
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    assertFalse(resolution === 0n, 'Resolution cannot be 0');

    // |resolution| is in ns/px we want # ns for 10px window:
    // Max value with 1 so we don't end up with resolution 0.
    const bucketSize = resolution * 10n;
    const windowStart = Time.quant(start, bucketSize);
    const windowDur = BigintMath.max(1n, end - windowStart);

    await this.engine.query(`update ${this.tableName('window')} set
      window_start=${windowStart},
      window_dur=${windowDur},
      quantum=${bucketSize}
      where rowid = 0;`);

    return this.computeSummary(windowStart, end, resolution, bucketSize);
  }

  private async computeSummary(
    start: time, end: time, resolution: duration,
    bucketSize: duration): Promise<Data> {
    const duration = end - start;
    const numBuckets = Math.min(Number(duration / bucketSize), LIMIT);

    const query = `select
      quantum_ts as bucket,
      sum(dur)/cast(${bucketSize} as float) as utilization
      from ${this.tableName('span')}
      group by quantum_ts
      limit ${LIMIT}`;

    const summary: Data = {
      start,
      end,
      resolution,
      length: numBuckets,
      bucketSize,
      utilizations: new Float64Array(numBuckets),
    };

    const queryRes = await this.engine.query(query);
    const it = queryRes.iter({bucket: NUM, utilization: NUM});
    for (; it.valid(); it.next()) {
      const bucket = it.bucket;
      if (bucket > numBuckets) {
        continue;
      }
      summary.utilizations[bucket] = it.utilization;
    }

    return summary;
  }

  async onDestroy(): Promise<void> {
    if (this.engine.isAlive) {
      await this.engine.query(`drop table if exists ${
        this.tableName(
          'window')}; drop table if exists ${this.tableName('span')}`);
    }
    this.fetcher.dispose();
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  render(ctx: CanvasRenderingContext2D, size: PanelSize): void {
    const {
      visibleTimeScale,
    } = globals.timeline;
    const data = this.fetcher.data;
    if (data === undefined) return;  // Can't possibly draw anything.

    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      visibleTimeScale.timeToPx(data.start),
      visibleTimeScale.timeToPx(data.end));

    this.renderSummary(ctx, data);
  }

  // TODO(dproy): Dedup with CPU slices.
  renderSummary(ctx: CanvasRenderingContext2D, data: Data): void {
    const {visibleTimeScale} = globals.timeline;
    const startPx = 0;
    const bottomY = TRACK_HEIGHT;

    let lastX = startPx;
    let lastY = bottomY;

    // TODO(hjd): Dedupe this math.
    const color = colorForTid(this.config.pidForColor);
    ctx.fillStyle = color.base.cssString;
    ctx.beginPath();
    ctx.moveTo(lastX, lastY);
    for (let i = 0; i < data.utilizations.length; i++) {
      // TODO(dproy): Investigate why utilization is > 1 sometimes.
      const utilization = Math.min(data.utilizations[i], 1);
      const startTime = Time.fromRaw(BigInt(i) * data.bucketSize + data.start);

      lastX = Math.floor(visibleTimeScale.timeToPx(startTime));

      ctx.lineTo(lastX, lastY);
      lastY = MARGIN_TOP + Math.round(SUMMARY_HEIGHT * (1 - utilization));
      ctx.lineTo(lastX, lastY);
    }
    ctx.lineTo(lastX, bottomY);
    ctx.closePath();
    ctx.fill();
  }
}
