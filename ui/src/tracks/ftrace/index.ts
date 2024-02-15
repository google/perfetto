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

import m from 'mithril';

import {duration, Time, time} from '../../base/time';
import {colorForFtrace} from '../../common/colorizer';
import {LIMIT, TrackData} from '../../common/track_data';
import {TimelineFetcher} from '../../common/track_helper';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {FtracePanel} from '../../frontend/ftrace_panel';
import {globals} from '../../frontend/globals';
import {PanelSize} from '../../frontend/panel';
import {
  EngineProxy,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  Track,
} from '../../public';
import {LONG, NUM, STR} from '../../trace_processor/query_result';

export const FTRACE_RAW_TRACK_KIND = 'FtraceRawTrack';

export interface Data extends TrackData {
  timestamps: BigInt64Array;
  names: string[];
}

export interface Config {
  cpu?: number;
}

const MARGIN = 2;
const RECT_HEIGHT = 18;
const TRACK_HEIGHT = (RECT_HEIGHT) + (2 * MARGIN);

class FtraceRawTrack implements Track {
  private fetcher = new TimelineFetcher(this.onBoundsChange.bind(this));

  constructor(private engine: EngineProxy, private cpu: number) {}

  async onUpdate(): Promise<void> {
    await this.fetcher.requestDataForCurrentTime();
  }

  async onDestroy?(): Promise<void> {
    this.fetcher.dispose();
  }

  getHeight(): number {
    return TRACK_HEIGHT;
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    const excludeList = Array.from(globals.state.ftraceFilter.excludedNames);
    const excludeListSql = excludeList.map((s) => `'${s}'`).join(',');
    const cpuFilter = this.cpu === undefined ? '' : `and cpu = ${this.cpu}`;

    const queryRes = await this.engine.query(`
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

  render(ctx: CanvasRenderingContext2D, size: PanelSize): void {
    const {
      visibleTimeScale,
    } = globals.timeline;

    const data = this.fetcher.data;

    if (data === undefined) return;  // Can't possibly draw anything.

    const dataStartPx = visibleTimeScale.timeToPx(data.start);
    const dataEndPx = visibleTimeScale.timeToPx(data.end);

    checkerboardExcept(
      ctx, this.getHeight(), 0, size.width, dataStartPx, dataEndPx);

    const diamondSideLen = RECT_HEIGHT / Math.sqrt(2);

    for (let i = 0; i < data.timestamps.length; i++) {
      const name = data.names[i];
      ctx.fillStyle = colorForFtrace(name).base.cssString;
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
}

class FtraceRawPlugin implements Plugin {
  onActivate(_: PluginContext) {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const cpus = await this.lookupCpuCores(ctx.engine);
    for (const cpuNum of cpus) {
      const uri = `perfetto.FtraceRaw#cpu${cpuNum}`;

      ctx.registerTrack({
        uri,
        displayName: `Ftrace Track for CPU ${cpuNum}`,
        kind: FTRACE_RAW_TRACK_KIND,
        cpu: cpuNum,
        trackFactory: () => {
          return new FtraceRawTrack(ctx.engine, cpuNum);
        },
      });
    }

    const ftraceTabUri = 'perfetto.FtraceRaw#FtraceEventsTab';

    ctx.registerTab({
      uri: ftraceTabUri,
      isEphemeral: false,
      content: {
        render: () => m(FtracePanel),
        getTitle: () => 'Ftrace Events',
      },
    });

    if (await this.hasFtrace(ctx.engine)) {
      ctx.addDefaultTab(ftraceTabUri);
    }

    ctx.registerCommand({
      id: 'perfetto.FtraceRaw#ShowFtraceTab',
      name: 'Show Ftrace Tab',
      callback: () => {
        ctx.tabs.showTab(ftraceTabUri);
      },
    });
  }

  private async hasFtrace(engine: EngineProxy): Promise<boolean> {
    // Check if we have any ftrace events at all
    const query = `
      select
        *
      from ftrace_event
      limit 1`;

    const res = await engine.query(query);
    return res.numRows() > 0;
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

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.FtraceRaw',
  plugin: FtraceRawPlugin,
};
