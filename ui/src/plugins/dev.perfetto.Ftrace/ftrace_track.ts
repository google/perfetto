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
import {materialColorScheme} from '../../components/colorizer';
import {LIMIT} from '../../components/tracks/track_data';
import {Store, TimelineFetcher} from '../../components/tracks/track_helper';
import {checkerboardExcept} from '../../components/checkerboard';
import {TrackData} from '../../components/tracks/track_data';
import {Engine} from '../../trace_processor/engine';
import {TrackRenderer} from '../../public/track';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {FtraceFilter} from './common';
import {Monitor} from '../../base/monitor';
import {TrackRenderContext} from '../../public/track';
import {SourceDataset} from '../../trace_processor/dataset';

const MARGIN = 2;
const RECT_HEIGHT = 18;
const RECT_WIDTH = 8;
const TRACK_HEIGHT = RECT_HEIGHT + 2 * MARGIN;

interface Data extends TrackData {
  events: Array<{
    timestamp: time;
    color: string;
  }>;
}

export interface Config {
  cpu?: number;
}

export class FtraceRawTrack implements TrackRenderer {
  private fetcher = new TimelineFetcher(this.onBoundsChange.bind(this));
  private engine: Engine;
  private ucpu: number;
  private store: Store<FtraceFilter>;
  private readonly monitor: Monitor;

  constructor(engine: Engine, ucpu: number, store: Store<FtraceFilter>) {
    this.engine = engine;
    this.ucpu = ucpu;
    this.store = store;

    this.monitor = new Monitor([() => store.state]);
  }

  getDataset() {
    return new SourceDataset({
      // 'ftrace_event' doesn't have a dur column, but injecting dur=0 (all
      // ftrace events are effectively 'instant') allows us to participate in
      // generic slice aggregations
      src: 'select id, ts, 0 as dur, name, ucpu from ftrace_event',
      schema: {
        id: NUM,
        name: STR,
        ts: LONG,
        dur: LONG,
      },
      filter: {
        col: 'ucpu',
        eq: this.ucpu,
      },
    });
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
    const cpuFilter = this.ucpu === undefined ? '' : `and ucpu = ${this.ucpu}`;

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

    const it = queryRes.iter({tsQuant: LONG, name: STR});
    const events = [];
    for (let row = 0; it.valid(); it.next(), row++) {
      events.push({
        timestamp: Time.fromRaw(it.tsQuant),
        color: materialColorScheme(it.name).base.cssString,
      });
    }
    return {
      start,
      end,
      resolution,
      length: rowCount,
      events,
    };
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
    for (const e of data.events) {
      ctx.fillStyle = e.color;
      const xPos = Math.floor(timescale.timeToPx(e.timestamp));
      ctx.fillRect(xPos - RECT_WIDTH / 2, MARGIN, RECT_WIDTH, RECT_HEIGHT);
    }
  }
}
