// Copyright (C) 2024 The Android Open Source Project
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

import {duration, Time, time} from '../../base/time';
import {colorForFtrace} from '../../core/colorizer';
import {LIMIT} from '../../common/track_data';
import {Store, TimelineFetcher} from '../../common/track_helper';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {TrackData} from '../../common/track_data';
import {Engine} from '../../trace_processor/engine';
import {Track} from '../../public/track';
import {LONG, STR} from '../../trace_processor/query_result';
import {FtraceFilter} from './common';
import {Monitor} from '../../base/monitor';
import {TrackRenderContext} from '../../public/track';

const MARGIN = 2;
const RECT_HEIGHT = 18;
const TRACK_HEIGHT = RECT_HEIGHT + 2 * MARGIN;

export interface Data extends TrackData {
  timestamps: BigInt64Array;
  names: string[];
}

export interface Config {
  cpu?: number;
}

export class FtraceRawTrack implements Track {
  private fetcher = new TimelineFetcher(this.onBoundsChange.bind(this));
  private engine: Engine;
  private cpu: number;
  private store: Store<FtraceFilter>;
  private readonly monitor: Monitor;

  constructor(engine: Engine, cpu: number, store: Store<FtraceFilter>) {
    this.engine = engine;
    this.cpu = cpu;
    this.store = store;

    this.monitor = new Monitor([() => store.state]);
  }

  async onUpdate({
    visibleWindow,
    resolution,
  }: TrackRenderContext): Promise<void> {
    this.monitor.ifStateChanged(() => {
      this.fetcher.invalidate();
    });
    await this.fetcher.requestData(visibleWindow.toTimeSpan(), resolution);
  }

  async onDestroy?(): Promise<void> {
    this.fetcher[Symbol.dispose]();
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  async onBoundsChange(
    start: time,
    end: time,
    resolution: duration,
  ): Promise<Data> {
    const excludeList = Array.from(this.store.state.excludeList);
    const excludeListSql = excludeList.map((s) => `'${s}'`).join(',');
    const cpuFilter = this.cpu === undefined ? '' : `and cpu = ${this.cpu}`;

    const queryRes = await this.engine.query(`
      select
        cast(ts / ${resolution} as integer) * ${resolution} as tsQuant,
        name
      from ftrace_event
      where
        name not in (${excludeListSql}) and
        ts >= ${start} and ts <= ${end} ${cpuFilter}
      group by tsQuant
      order by tsQuant limit ${LIMIT};`);

    const rowCount = queryRes.numRows();
    const result: Data = {
      start,
      end,
      resolution,
      length: rowCount,
      timestamps: new BigInt64Array(rowCount),
      names: [],
    };

    const it = queryRes.iter({tsQuant: LONG, name: STR});
    for (let row = 0; it.valid(); it.next(), row++) {
      result.timestamps[row] = it.tsQuant;
      result.names[row] = it.name;
    }
    return result;
  }

  render({ctx, size, timescale}: TrackRenderContext): void {
    const data = this.fetcher.data;

    if (data === undefined) return; // Can't possibly draw anything.

    const dataStartPx = timescale.timeToPx(data.start);
    const dataEndPx = timescale.timeToPx(data.end);

    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      dataStartPx,
      dataEndPx,
    );

    const diamondSideLen = RECT_HEIGHT / Math.sqrt(2);

    for (let i = 0; i < data.timestamps.length; i++) {
      const name = data.names[i];
      ctx.fillStyle = colorForFtrace(name).base.cssString;
      const timestamp = Time.fromRaw(data.timestamps[i]);
      const xPos = Math.floor(timescale.timeToPx(timestamp));

      // Draw a diamond over the event
      ctx.save();
      ctx.translate(xPos, MARGIN);
      ctx.rotate(Math.PI / 4);
      ctx.fillRect(0, 0, diamondSideLen, diamondSideLen);
      ctx.restore();
    }
  }
}
