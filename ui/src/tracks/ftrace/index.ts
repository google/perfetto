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

import {Vnode} from 'mithril';

import {colorForString} from '../../common/colorizer';
import {LONG, NUM, STR} from '../../common/query_result';
import {duration, Time, time} from '../../common/time';
import {
  TrackAdapter,
  TrackControllerAdapter,
  TrackWithControllerAdapter,
} from '../../common/track_adapter';
import {LIMIT, TrackData} from '../../common/track_data';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {globals} from '../../frontend/globals';
import {NewTrackArgs} from '../../frontend/track';
import {
  EngineProxy,
  Plugin,
  PluginContext,
  PluginInfo,
  TracePluginContext,
} from '../../public';


export interface Data extends TrackData {
  timestamps: BigInt64Array;
  names: string[];
}

export interface Config {
  cpu?: number;
}

export const FTRACE_RAW_TRACK_KIND = 'FtraceRawTrack';

const MARGIN = 2;
const RECT_HEIGHT = 18;
const TRACK_HEIGHT = (RECT_HEIGHT) + (2 * MARGIN);

class FtraceRawTrackController extends TrackControllerAdapter<Config, Data> {
  static readonly kind = FTRACE_RAW_TRACK_KIND;

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    const excludeList = Array.from(globals.state.ftraceFilter.excludedNames);
    const excludeListSql = excludeList.map((s) => `'${s}'`).join(',');
    const cpuFilter =
        this.config.cpu === undefined ? '' : `and cpu = ${this.config.cpu}`;

    const queryRes = await this.query(`
      select
        cast(ts / ${resolution} as integer) * ${resolution} as tsQuant,
        type,
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

    const it = queryRes.iter(
        {tsQuant: LONG, type: STR, name: STR},
    );
    for (let row = 0; it.valid(); it.next(), row++) {
      result.timestamps[row] = it.tsQuant;
      result.names[row] = it.name;
    }
    return result;
  }
}

export class FtraceRawTrack extends TrackAdapter<Config, Data> {
  static readonly kind = FTRACE_RAW_TRACK_KIND;
  constructor(args: NewTrackArgs) {
    super(args);
  }

  static create(args: NewTrackArgs): FtraceRawTrack {
    return new FtraceRawTrack(args);
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

    const dataStartPx = visibleTimeScale.timeToPx(data.start);
    const dataEndPx = visibleTimeScale.timeToPx(data.end);
    const visibleStartPx = windowSpan.start;
    const visibleEndPx = windowSpan.end;

    checkerboardExcept(
        ctx,
        this.getHeight(),
        visibleStartPx,
        visibleEndPx,
        dataStartPx,
        dataEndPx);

    const diamondSideLen = RECT_HEIGHT / Math.sqrt(2);

    for (let i = 0; i < data.timestamps.length; i++) {
      const name = data.names[i];
      const color = colorForString(name);
      const hsl = `hsl(
        ${color.h},
        ${color.s - 20}%,
        ${Math.min(color.l + 10, 60)}%
      )`;
      ctx.fillStyle = hsl;
      const timestamp = Time.fromRaw(data.timestamps[i]);
      const xPos = Math.floor(visibleTimeScale.timeToPx(timestamp));

      // Draw a diamond over the event
      ctx.save();
      ctx.translate(xPos, MARGIN);
      ctx.rotate(Math.PI / 4);
      ctx.fillRect(0, 0, diamondSideLen, diamondSideLen);
      ctx.restore();
    }
  }

  getContextMenu(): Vnode<any, {}>|null {
    return null;
  }
}

class FtraceRawPlugin implements Plugin {
  onActivate(_: PluginContext) {}

  async onTraceLoad(ctx: TracePluginContext): Promise<void> {
    const cpus = await this.lookupCpuCores(ctx.engine);
    for (const cpuNum of cpus) {
      const uri = `perfetto.FtraceRaw#cpu${cpuNum}`;
      const config: Config = {cpu: cpuNum};

      ctx.addTrack({
        uri,
        displayName: `Ftrace Track for CPU ${cpuNum}`,
        trackFactory: () => {
          return new TrackWithControllerAdapter<Config, Data>(
              ctx.engine,
              uri,
              config,
              FtraceRawTrack,
              FtraceRawTrackController);
        },
      });
    }
  }

  private async lookupCpuCores(engine: EngineProxy): Promise<number[]> {
    const query = 'select distinct cpu from ftrace_event';

    const result = await engine.query(query);
    const it = result.iter({cpu: NUM});

    const cpuCores: number[] = [];

    for (; it.valid(); it.next()) {
      cpuCores.push(it.cpu);
    }

    return cpuCores;
  }
}

export const plugin: PluginInfo = {
  pluginId: 'perfetto.FtraceRaw',
  plugin: FtraceRawPlugin,
};
