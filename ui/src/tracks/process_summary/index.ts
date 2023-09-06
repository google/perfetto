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

import {BigintMath} from '../../base/bigint_math';
import {assertFalse} from '../../base/logging';
import {colorForTid} from '../../common/colorizer';
import {PluginContext} from '../../common/plugin_api';
import {NUM} from '../../common/query_result';
import {TPDuration, TPTime} from '../../common/time';
import {TrackData} from '../../common/track_data';
import {LIMIT} from '../../common/track_data';
import {TrackController} from '../../controller/track_controller';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {NewTrackArgs, Track} from '../../frontend/track';

export const PROCESS_SUMMARY_TRACK = 'ProcessSummaryTrack';

// TODO(dproy): Consider deduping with CPU summary data.
export interface Data extends TrackData {
  bucketSize: TPDuration;
  utilizations: Float64Array;
}

export interface Config {
  pidForColor: number;
  upid: number|null;
  utid: number;
}

// This is the summary displayed when a process only contains chrome slices
// and no cpu scheduling.
class ProcessSummaryTrackController extends TrackController<Config, Data> {
  static readonly kind = PROCESS_SUMMARY_TRACK;
  private setup = false;

  async onBoundsChange(start: TPTime, end: TPTime, resolution: TPDuration):
      Promise<Data> {
    assertFalse(resolution === 0n, 'Resolution cannot be 0');

    if (this.setup === false) {
      await this.query(
          `create virtual table ${this.tableName('window')} using window;`);

      let utids = [this.config.utid];
      if (this.config.upid) {
        const threadQuery = await this.query(
            `select utid from thread where upid=${this.config.upid}`);
        utids = [];
        for (const it = threadQuery.iter({utid: NUM}); it.valid(); it.next()) {
          utids.push(it.utid);
        }
      }

      const trackQuery = await this.query(
          `select id from thread_track where utid in (${utids.join(',')})`);
      const tracks = [];
      for (const it = trackQuery.iter({id: NUM}); it.valid(); it.next()) {
        tracks.push(it.id);
      }

      const processSliceView = this.tableName('process_slice_view');
      await this.query(
          `create view ${processSliceView} as ` +
          // 0 as cpu is a dummy column to perform span join on.
          `select ts, dur/${utids.length} as dur ` +
          `from slice s ` +
          `where depth = 0 and track_id in ` +
          `(${tracks.join(',')})`);
      await this.query(`create virtual table ${this.tableName('span')}
          using span_join(${processSliceView},
                          ${this.tableName('window')});`);
      this.setup = true;
    }

    // |resolution| is in ns/px we want # ns for 10px window:
    // Max value with 1 so we don't end up with resolution 0.
    const bucketSize = resolution * 10n;
    const windowStart = BigintMath.quant(start, bucketSize);
    const windowDur = BigintMath.max(1n, end - windowStart);

    await this.query(`update ${this.tableName('window')} set
      window_start=${windowStart},
      window_dur=${windowDur},
      quantum=${bucketSize}
      where rowid = 0;`);

    return this.computeSummary(windowStart, end, resolution, bucketSize);
  }

  private async computeSummary(
      start: TPTime, end: TPTime, resolution: TPDuration,
      bucketSize: TPDuration): Promise<Data> {
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

    const queryRes = await this.query(query);
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

  onDestroy(): void {
    if (this.setup) {
      this.query(`drop table if exists ${this.tableName('window')}`);
      this.query(`drop table if exists ${this.tableName('span')}`);
      this.setup = false;
    }
  }
}

const MARGIN_TOP = 5;
const RECT_HEIGHT = 30;
const TRACK_HEIGHT = MARGIN_TOP * 2 + RECT_HEIGHT;
const SUMMARY_HEIGHT = TRACK_HEIGHT - MARGIN_TOP;

class ProcessSummaryTrack extends Track<Config, Data> {
  static readonly kind = PROCESS_SUMMARY_TRACK;
  static create(args: NewTrackArgs): ProcessSummaryTrack {
    return new ProcessSummaryTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    const {
      visibleTimeScale,
      windowSpan,
    } = globals.frontendLocalState;
    const data = this.data();
    if (data === undefined) return;  // Can't possibly draw anything.

    checkerboardExcept(
        ctx,
        this.getHeight(),
        windowSpan.start,
        windowSpan.end,
        visibleTimeScale.tpTimeToPx(data.start),
        visibleTimeScale.tpTimeToPx(data.end));

    this.renderSummary(ctx, data);
  }

  // TODO(dproy): Dedup with CPU slices.
  renderSummary(ctx: CanvasRenderingContext2D, data: Data): void {
    const {visibleTimeScale, windowSpan} = globals.frontendLocalState;
    const startPx = windowSpan.start;
    const bottomY = TRACK_HEIGHT;

    let lastX = startPx;
    let lastY = bottomY;

    // TODO(hjd): Dedupe this math.
    const color = colorForTid(this.config.pidForColor);
    color.l = Math.min(color.l + 10, 60);
    color.s -= 20;

    ctx.fillStyle = `hsl(${color.h}, ${color.s}%, ${color.l}%)`;
    ctx.beginPath();
    ctx.moveTo(lastX, lastY);
    for (let i = 0; i < data.utilizations.length; i++) {
      // TODO(dproy): Investigate why utilization is > 1 sometimes.
      const utilization = Math.min(data.utilizations[i], 1);
      const startTime = BigInt(i) * data.bucketSize + data.start;

      lastX = Math.floor(visibleTimeScale.tpTimeToPx(startTime));

      ctx.lineTo(lastX, lastY);
      lastY = MARGIN_TOP + Math.round(SUMMARY_HEIGHT * (1 - utilization));
      ctx.lineTo(lastX, lastY);
    }
    ctx.lineTo(lastX, bottomY);
    ctx.closePath();
    ctx.fill();
  }
}

export function activate(ctx: PluginContext) {
  ctx.registerTrack(ProcessSummaryTrack);
  ctx.registerTrackController(ProcessSummaryTrackController);
}

export const plugin = {
  pluginId: 'perfetto.ProcessSummary',
  activate,
};
