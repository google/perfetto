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

import {
  BaseCounterTrack,
  CounterOptions,
} from '../../components/tracks/base_counter_track';
import {
  CPUSS_ESTIMATE_TRACK_KIND,
  GPUSS_ESTIMATE_TRACK_KIND,
} from './track_kinds';
import {createWattsonAggregationToTabAdaptor} from './aggregation_panel';
import {createQuerySliceTrack} from '../../components/tracks/query_slice_track';
import {Engine} from '../../trace_processor/engine';
import {NUM} from '../../trace_processor/query_result';
import {PerfettoPlugin} from '../../public/plugin';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {WattsonEstimateSelectionAggregator} from './estimate_aggregator';
import {WattsonPackageSelectionAggregator} from './package_aggregator';
import {WattsonProcessSelectionAggregator} from './process_aggregator';
import {WattsonThreadSelectionAggregator} from './thread_aggregator';

export default class implements PerfettoPlugin {
  static readonly id = `org.kernel.Wattson`;

  async onTraceLoad(ctx: Trace): Promise<void> {
    const cpuSupported = await hasWattsonCpuSupport(ctx.engine);
    const gpuSupported = await hasWattsonGpuSupport(ctx.engine);
    // Short circuit if Wattson is not supported for this Perfetto trace
    if (!(cpuSupported || gpuSupported)) return;

    const group = new TrackNode({title: 'Wattson', isSummary: true});
    ctx.workspace.addChildInOrder(group);

    // Add Wattson markers window track if markers are present
    const checkValue = await ctx.engine.query(`
        INCLUDE PERFETTO MODULE wattson.utils;
        SELECT COUNT(*) as numRows from _wattson_markers_window
    `);
    if (checkValue.firstRow({numRows: NUM}).numRows > 0) {
      const uri = `/wattson/markers_window`;
      const title = `Wattson markers window`;
      const track = await createQuerySliceTrack({
        trace: ctx,
        uri,
        data: {
          sqlSource: `SELECT ts, dur, name FROM _wattson_markers_window`,
        },
      });
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          kind: SLICE_TRACK_KIND,
        },
        track,
      });
      group.addChildInOrder(new TrackNode({uri, title}));
    }
    if (cpuSupported) {
      await addWattsonCpuElements(ctx, group);
    }
    if (gpuSupported) {
      await addWattsonGpuElements(ctx, group);
    }
  }
}

class CpuSubsystemEstimateTrack extends BaseCounterTrack {
  readonly queryKey: string;

  constructor(trace: Trace, uri: string, queryKey: string) {
    super(trace, uri);
    this.queryKey = queryKey;
  }

  async onInit() {
    await this.engine.query(`INCLUDE PERFETTO MODULE wattson.estimates;`);
  }

  protected getDefaultCounterOptions(): CounterOptions {
    const options = super.getDefaultCounterOptions();
    options.yRangeSharingKey = `CpuSubsystem`;
    options.unit = `mW`;
    return options;
  }

  getSqlSource() {
    return `select ts, ${this.queryKey} as value from _system_state_mw`;
  }
}

class GpuSubsystemEstimateTrack extends BaseCounterTrack {
  readonly queryKey: string;

  constructor(trace: Trace, uri: string, queryKey: string) {
    super(trace, uri);
    this.queryKey = queryKey;
  }

  async onInit() {
    await this.engine.query(`INCLUDE PERFETTO MODULE wattson.gpu.estimates;`);
  }

  protected getDefaultCounterOptions(): CounterOptions {
    const options = super.getDefaultCounterOptions();
    options.yRangeSharingKey = `GpuSubsystem`;
    options.unit = `mW`;
    return options;
  }

  getSqlSource() {
    return `select ts, ${this.queryKey} as value from _gpu_estimates`;
  }
}

async function hasWattsonCpuSupport(engine: Engine): Promise<boolean> {
  // These tables are hard requirements and are the bare minimum needed for
  // Wattson to run, so check that these tables are populated
  const queryChecks: string[] = [
    `
    INCLUDE PERFETTO MODULE wattson.device_infos;
    SELECT COUNT(*) as numRows FROM _wattson_device
    `,
    `
    INCLUDE PERFETTO MODULE linux.cpu.frequency;
    SELECT COUNT(*) as numRows FROM cpu_frequency_counters
    `,
    `
    INCLUDE PERFETTO MODULE linux.cpu.idle;
    SELECT COUNT(*) as numRows FROM cpu_idle_counters
    `,
  ];
  for (const queryCheck of queryChecks) {
    const checkValue = await engine.query(queryCheck);
    if (checkValue.firstRow({numRows: NUM}).numRows === 0) return false;
  }

  return true;
}

async function hasWattsonGpuSupport(engine: Engine): Promise<boolean> {
  // These tables are hard requirements and are the bare minimum needed for
  // Wattson to run, so check that these tables are populated
  const queryChecks: string[] = [
    `
    INCLUDE PERFETTO MODULE android.gpu.frequency;
    SELECT COUNT(*) as numRows FROM android_gpu_frequency
    `,
    `
    INCLUDE PERFETTO MODULE android.gpu.mali_power_state;
    SELECT COUNT(*) as numRows FROM android_mali_gpu_power_state
    `,
  ];
  for (const queryCheck of queryChecks) {
    const checkValue = await engine.query(queryCheck);
    if (checkValue.firstRow({numRows: NUM}).numRows === 0) return false;
  }

  return true;
}

async function addWattsonCpuElements(ctx: Trace, group: TrackNode) {
  // ctx.traceInfo.cpus contains all cpus seen from all events. Filter the set
  // if it's seen in sched slices.
  const queryRes = await ctx.engine.query(
    `select distinct ucpu from sched order by ucpu;`,
  );
  const ucpus = new Set<number>();
  for (
    const it = queryRes.iter({ucpu: NUM});
    it.valid() as boolean;
    it.next()
  ) {
    ucpus.add(it.ucpu);
  }

  // CPUs estimate as part of CPU subsystem
  const cpus = ctx.traceInfo.cpus.filter((cpu) => ucpus.has(cpu.ucpu));
  for (const cpu of cpus) {
    const queryKey = `cpu${cpu.ucpu}_mw`;
    const uri = `/wattson/cpu_subsystem_estimate_cpu${cpu.ucpu}`;
    const title = `Cpu${cpu.toString()} Estimate`;
    ctx.tracks.registerTrack({
      uri,
      title,
      track: new CpuSubsystemEstimateTrack(ctx, uri, queryKey),
      tags: {
        kind: CPUSS_ESTIMATE_TRACK_KIND,
        wattson: `CPU${cpu.ucpu}`,
        groupName: `Wattson`,
      },
    });
    group.addChildInOrder(new TrackNode({uri, title}));
  }

  const uri = `/wattson/cpu_subsystem_estimate_dsu_scu`;
  const title = `DSU/SCU Estimate`;
  ctx.tracks.registerTrack({
    uri,
    title,
    track: new CpuSubsystemEstimateTrack(ctx, uri, `dsu_scu_mw`),
    tags: {
      kind: CPUSS_ESTIMATE_TRACK_KIND,
      wattson: 'Dsu_Scu',
      groupName: `Wattson`,
    },
  });
  group.addChildInOrder(new TrackNode({uri, title}));

  // Register selection aggregators.
  // NOTE: the registration order matters because the laste two aggregators
  // depend on views created by the first two.
  ctx.selection.registerAreaSelectionTab(
    createWattsonAggregationToTabAdaptor(
      ctx,
      new WattsonEstimateSelectionAggregator(),
    ),
  );
  ctx.selection.registerAreaSelectionTab(
    createWattsonAggregationToTabAdaptor(
      ctx,
      new WattsonThreadSelectionAggregator(),
    ),
  );
  ctx.selection.registerAreaSelectionTab(
    createWattsonAggregationToTabAdaptor(
      ctx,
      new WattsonProcessSelectionAggregator(),
    ),
  );
  ctx.selection.registerAreaSelectionTab(
    createWattsonAggregationToTabAdaptor(
      ctx,
      new WattsonPackageSelectionAggregator(),
    ),
  );
}

async function addWattsonGpuElements(ctx: Trace, group: TrackNode) {
  const uri = `/wattson/gpu_subsystem_estimate`;
  const title = `GPU Estimate`;
  ctx.tracks.registerTrack({
    uri,
    title,
    track: new GpuSubsystemEstimateTrack(ctx, uri, `gpu_mw`),
    tags: {
      kind: GPUSS_ESTIMATE_TRACK_KIND,
      wattson: 'Gpu',
      groupName: `Wattson`,
    },
  });
  group.addChildInOrder(new TrackNode({uri, title}));
}
