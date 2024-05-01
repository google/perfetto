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

import m from 'mithril';

import {Time} from '../../base/time';
import {Actions} from '../../common/actions';
import {CounterDetailsPanel} from '../../frontend/counter_panel';
import {globals} from '../../frontend/globals';
import {
  NUM_NULL,
  STR_NULL,
  LONG,
  LONG_NULL,
  NUM,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
  PrimaryTrackSortKey,
  STR,
} from '../../public';
import {getTrackName} from '../../public/utils';
import {
  BaseCounterTrack,
  BaseCounterTrackArgs,
  CounterOptions,
} from '../../frontend/base_counter_track';

export const COUNTER_TRACK_KIND = 'CounterTrack';

const NETWORK_TRACK_REGEX = new RegExp('^.* (Received|Transmitted)( KB)?$');
const ENTITY_RESIDENCY_REGEX = new RegExp('^Entity residency:');

type Modes = CounterOptions['yMode'];

// Sets the default 'mode' for counter tracks. If the regex matches
// then the paired mode is used. Entries are in priority order so the
// first match wins.
const COUNTER_REGEX: [RegExp, Modes][] = [
  // Power counters make more sense in rate mode since you're typically
  // interested in the slope of the graph rather than the absolute
  // value.
  [new RegExp('^power..*$'), 'rate'],
  // Same for cumulative PSI stall time counters, e.g., psi.cpu.some.
  [new RegExp('^psi..*$'), 'rate'],
  // Same for network counters.
  [NETWORK_TRACK_REGEX, 'rate'],
  // Entity residency
  [ENTITY_RESIDENCY_REGEX, 'rate'],
];

function getCounterMode(name: string): Modes | undefined {
  for (const [re, mode] of COUNTER_REGEX) {
    if (name.match(re)) {
      return mode;
    }
  }
  return undefined;
}

function getDefaultCounterOptions(name: string): Partial<CounterOptions> {
  const options: Partial<CounterOptions> = {};
  options.yMode = getCounterMode(name);

  if (name.endsWith('_pct')) {
    options.yOverrideMinimum = 0;
    options.yOverrideMaximum = 100;
    options.unit = '%';
  }

  if (name.startsWith('power.')) {
    options.yRangeSharingKey = 'power';
  }

  if (name.startsWith('mem.')) {
    options.yRangeSharingKey = 'mem';
  }

  if (name.startsWith('battery_stats.')) {
    options.yRangeSharingKey = 'battery_stats';
  }

  // All 'Entity residency: foo bar1234' tracks should share a y-axis
  // with 'Entity residency: foo baz5678' etc tracks:
  {
    const r = new RegExp('Entity residency: ([^ ]+) ');
    const m = r.exec(name);
    if (m) {
      options.yRangeSharingKey = `entity-residency-${m[1]}`;
    }
  }

  {
    const r = new RegExp('GPU .* Frequency');
    const m = r.exec(name);
    if (m) {
      options.yRangeSharingKey = 'gpu-frequency';
    }
  }

  return options;
}

interface TraceProcessorCounterTrackArgs extends BaseCounterTrackArgs {
  trackId: number;
  rootTable?: string;
}

export class TraceProcessorCounterTrack extends BaseCounterTrack {
  private trackId: number;
  private rootTable: string;

  constructor(args: TraceProcessorCounterTrackArgs) {
    super(args);
    this.trackId = args.trackId;
    this.rootTable = args.rootTable ?? 'counter';
  }

  getSqlSource() {
    return `select ts, value from ${this.rootTable} where track_id = ${this.trackId}`;
  }

  onMouseClick({x}: {x: number}): boolean {
    const {visibleTimeScale} = globals.timeline;
    const time = visibleTimeScale.pxToHpTime(x).toTime('floor');

    const query = `
      select
        id,
        ts as leftTs,
        (
          select ts
          from ${this.rootTable}
          where
            track_id = ${this.trackId}
            and ts >= ${time}
          order by ts
          limit 1
        ) as rightTs
      from ${this.rootTable}
      where
        track_id = ${this.trackId}
        and ts < ${time}
      order by ts DESC
      limit 1
    `;

    this.engine.query(query).then((result) => {
      const it = result.iter({
        id: NUM,
        leftTs: LONG,
        rightTs: LONG_NULL,
      });
      if (!it.valid()) {
        return;
      }
      const trackKey = this.trackKey;
      const id = it.id;
      const leftTs = Time.fromRaw(it.leftTs);

      // TODO(stevegolton): Don't try to guess times and durations here, make it
      // obvious to the user that this counter sample has no duration as it's
      // the last one in the series
      const rightTs = Time.fromRaw(it.rightTs ?? leftTs);

      globals.makeSelection(
        Actions.selectCounter({
          leftTs,
          rightTs,
          id,
          trackKey,
        }),
      );
    });

    return true;
  }
}

class CounterPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    await this.addCounterTracks(ctx);
    await this.addGpuFrequencyTracks(ctx);
    await this.addCpuFreqLimitCounterTracks(ctx);
    await this.addCpuPerfCounterTracks(ctx);
    await this.addThreadCounterTracks(ctx);
    await this.addProcessCounterTracks(ctx);

    ctx.registerDetailsPanel({
      render: (sel) => {
        if (sel.kind === 'COUNTER') {
          return m(CounterDetailsPanel);
        } else {
          return undefined;
        }
      },
    });
  }

  private async addCounterTracks(ctx: PluginContextTrace) {
    const result = await ctx.engine.query(`
      select name, id, unit
      from (
        select name, id, unit
        from counter_track
        where type = 'counter_track'
        union
        select name, id, unit
        from gpu_counter_track
        where name != 'gpufreq'
      )
      order by name
    `);

    // Add global or GPU counter tracks that are not bound to any pid/tid.
    const it = result.iter({
      name: STR,
      unit: STR_NULL,
      id: NUM,
    });

    for (; it.valid(); it.next()) {
      const trackId = it.id;
      const displayName = it.name;
      const unit = it.unit ?? undefined;
      ctx.registerStaticTrack({
        uri: `perfetto.Counter#${trackId}`,
        displayName,
        kind: COUNTER_TRACK_KIND,
        trackIds: [trackId],
        trackFactory: (trackCtx) => {
          return new TraceProcessorCounterTrack({
            engine: ctx.engine,
            trackKey: trackCtx.trackKey,
            trackId,
            options: {
              ...getDefaultCounterOptions(displayName),
              unit,
            },
          });
        },
        sortKey: PrimaryTrackSortKey.COUNTER_TRACK,
      });
    }
  }

  async addCpuFreqLimitCounterTracks(ctx: PluginContextTrace): Promise<void> {
    const cpuFreqLimitCounterTracksSql = `
      select name, id
      from cpu_counter_track
      where name glob "Cpu * Freq Limit"
      order by name asc
    `;

    this.addCpuCounterTracks(ctx, cpuFreqLimitCounterTracksSql);
  }

  async addCpuPerfCounterTracks(ctx: PluginContextTrace): Promise<void> {
    // Perf counter tracks are bound to CPUs, follow the scheduling and
    // frequency track naming convention ("Cpu N ...").
    // Note: we might not have a track for a given cpu if no data was seen from
    // it. This might look surprising in the UI, but placeholder tracks are
    // wasteful as there's no way of collapsing global counter tracks at the
    // moment.
    const addCpuPerfCounterTracksSql = `
      select printf("Cpu %u %s", cpu, name) as name, id
      from perf_counter_track as pct
      order by perf_session_id asc, pct.name asc, cpu asc
    `;
    this.addCpuCounterTracks(ctx, addCpuPerfCounterTracksSql);
  }

  async addCpuCounterTracks(
    ctx: PluginContextTrace,
    sql: string,
  ): Promise<void> {
    const result = await ctx.engine.query(sql);

    const it = result.iter({
      name: STR,
      id: NUM,
    });

    for (; it.valid(); it.next()) {
      const name = it.name;
      const trackId = it.id;
      ctx.registerTrack({
        uri: `perfetto.Counter#cpu${trackId}`,
        displayName: name,
        kind: COUNTER_TRACK_KIND,
        trackIds: [trackId],
        trackFactory: (trackCtx) => {
          return new TraceProcessorCounterTrack({
            engine: ctx.engine,
            trackKey: trackCtx.trackKey,
            trackId: trackId,
            options: getDefaultCounterOptions(name),
          });
        },
      });
    }
  }

  async addThreadCounterTracks(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
      select
        thread_counter_track.name as trackName,
        utid,
        upid,
        tid,
        thread.name as threadName,
        thread_counter_track.id as trackId,
        thread.start_ts as startTs,
        thread.end_ts as endTs
      from thread_counter_track
      join thread using(utid)
      where thread_counter_track.name != 'thread_time'
    `);

    const it = result.iter({
      startTs: LONG_NULL,
      trackId: NUM,
      endTs: LONG_NULL,
      trackName: STR_NULL,
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const tid = it.tid;
      const trackId = it.trackId;
      const trackName = it.trackName;
      const threadName = it.threadName;
      const kind = COUNTER_TRACK_KIND;
      const name = getTrackName({
        name: trackName,
        utid,
        tid,
        kind,
        threadName,
        threadTrack: true,
      });
      ctx.registerTrack({
        uri: `perfetto.Counter#thread${trackId}`,
        displayName: name,
        kind,
        trackIds: [trackId],
        trackFactory: (trackCtx) => {
          return new TraceProcessorCounterTrack({
            engine: ctx.engine,
            trackKey: trackCtx.trackKey,
            trackId: trackId,
            options: getDefaultCounterOptions(name),
          });
        },
      });
    }
  }

  async addProcessCounterTracks(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
    select
      process_counter_track.id as trackId,
      process_counter_track.name as trackName,
      upid,
      process.pid,
      process.name as processName
    from process_counter_track
    join process using(upid);
  `);
    const it = result.iter({
      trackId: NUM,
      trackName: STR_NULL,
      upid: NUM,
      pid: NUM_NULL,
      processName: STR_NULL,
    });
    for (let i = 0; it.valid(); ++i, it.next()) {
      const trackId = it.trackId;
      const pid = it.pid;
      const trackName = it.trackName;
      const upid = it.upid;
      const processName = it.processName;
      const kind = COUNTER_TRACK_KIND;
      const name = getTrackName({
        name: trackName,
        upid,
        pid,
        kind,
        processName,
      });
      ctx.registerTrack({
        uri: `perfetto.Counter#process${trackId}`,
        displayName: name,
        kind: COUNTER_TRACK_KIND,
        trackIds: [trackId],
        trackFactory: (trackCtx) => {
          return new TraceProcessorCounterTrack({
            engine: ctx.engine,
            trackKey: trackCtx.trackKey,
            trackId: trackId,
            options: getDefaultCounterOptions(name),
          });
        },
      });
    }
  }

  private async addGpuFrequencyTracks(ctx: PluginContextTrace) {
    const engine = ctx.engine;
    const numGpus = await engine.getNumberOfGpus();

    for (let gpu = 0; gpu < numGpus; gpu++) {
      // Only add a gpu freq track if we have
      // gpu freq data.
      const freqExistsResult = await engine.query(`
      select id
      from gpu_counter_track
      where name = 'gpufreq' and gpu_id = ${gpu}
      limit 1;
    `);
      if (freqExistsResult.numRows() > 0) {
        const trackId = freqExistsResult.firstRow({id: NUM}).id;
        const uri = `perfetto.Counter#gpu_freq${gpu}`;
        const name = `Gpu ${gpu} Frequency`;
        ctx.registerTrack({
          uri,
          displayName: name,
          kind: COUNTER_TRACK_KIND,
          trackIds: [trackId],
          trackFactory: (trackCtx) => {
            return new TraceProcessorCounterTrack({
              engine: ctx.engine,
              trackKey: trackCtx.trackKey,
              trackId: trackId,
              options: getDefaultCounterOptions(name),
            });
          },
        });
      }
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Counter',
  plugin: CounterPlugin,
};
