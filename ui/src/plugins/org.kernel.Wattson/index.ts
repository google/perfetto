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

import {createAggregationTab} from '../../components/aggregation_adapter';
import {
  BaseCounterTrack,
  CounterOptions,
} from '../../components/tracks/base_counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {Engine} from '../../trace_processor/engine';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, LONG_NULL, NUM, STR} from '../../trace_processor/query_result';
import {WattsonEstimateSelectionAggregator} from './estimate_aggregator';
import {WattsonPackageSelectionAggregator} from './package_aggregator';
import {WattsonProcessSelectionAggregator} from './process_aggregator';
import {WattsonThreadSelectionAggregator} from './thread_aggregator';
import {
  CPUSS_ESTIMATE_TRACK_KIND,
  GPUSS_ESTIMATE_TRACK_KIND,
} from './track_kinds';
import SchedPlugin from '../dev.perfetto.Sched';
import {createCpuWarnings, hasWattsonSufficientCPUConfigs} from './warning';

export default class implements PerfettoPlugin {
  static readonly id = `org.kernel.Wattson`;
  static readonly dependencies = [SchedPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const markersSupported = await hasWattsonMarkersSupport(ctx.engine);
    const cpuSupported = await hasWattsonCpuSupport(ctx.engine);
    const gpuSupported = await hasWattsonGpuSupport(ctx.engine);
    const realCpuIdleCounters = await hasCpuIdleCounters(ctx.engine);
    const missingEvents = markersSupported
      ? await hasWattsonSufficientCPUConfigs(ctx.engine)
      : [];

    // Short circuit if Wattson is not supported for this Perfetto trace
    if (!(markersSupported || cpuSupported || gpuSupported)) return;

    const group = new TrackNode({name: 'Wattson', isSummary: true});
    ctx.defaultWorkspace.addChildInOrder(group);

    if (markersSupported) {
      await addWattsonMarkersElements(ctx, group);
    }
    if (cpuSupported || markersSupported) {
      await addWattsonCpuElements(
        ctx,
        group,
        missingEvents,
        realCpuIdleCounters,
      );
    }
    if (gpuSupported) {
      await addWattsonGpuElements(ctx, group);
    }
  }
}

class WattsonSubsystemEstimateTrack extends BaseCounterTrack {
  readonly queryKey: string;
  readonly yRangeKey: string;

  constructor(trace: Trace, uri: string, queryKey: string, yRangeKey: string) {
    super(trace, uri);
    this.queryKey = queryKey;
    this.yRangeKey = yRangeKey;
  }

  async onInit() {
    await this.engine.query(
      `INCLUDE PERFETTO MODULE wattson.ui.continuous_estimates;`,
    );
  }

  protected getDefaultCounterOptions(): CounterOptions {
    const options = super.getDefaultCounterOptions();
    options.yRangeSharingKey = this.yRangeKey;
    options.unit = `mW`;
    return options;
  }

  getSqlSource() {
    return `
      SELECT ts, ${this.queryKey} AS value
      FROM _system_state_${this.queryKey}
    `;
  }
}

async function hasCpuIdleCounters(engine: Engine): Promise<boolean> {
  const checkValue = await engine.query(`
      INCLUDE PERFETTO MODULE wattson.cpu.idle;
      SELECT COUNT(*) as numRows from _wattson_cpuidle_counters_exist
  `);
  return checkValue.firstRow({numRows: NUM}).numRows > 0;
}

async function hasWattsonMarkersSupport(engine: Engine): Promise<boolean> {
  const checkValue = await engine.query(`
      INCLUDE PERFETTO MODULE wattson.utils;
      SELECT COUNT(*) as numRows from _wattson_markers_window
  `);
  return checkValue.firstRow({numRows: NUM}).numRows > 0;
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
    INCLUDE PERFETTO MODULE wattson.cpu.idle;
    SELECT COUNT(*) as numRows FROM _adjusted_deep_idle
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

async function addWattsonMarkersElements(ctx: Trace, group: TrackNode) {
  const uri = `/wattson/markers_window`;
  const track = await SliceTrack.createMaterialized({
    trace: ctx,
    uri,
    dataset: new SourceDataset({
      schema: {
        ts: LONG,
        dur: LONG_NULL,
        name: STR,
      },
      src: '_wattson_markers_window',
    }),
    // Use default details panel
  });
  ctx.tracks.registerTrack({
    uri,
    tags: {
      kinds: [SLICE_TRACK_KIND],
    },
    renderer: track,
  });
  group.addChildInOrder(new TrackNode({uri, name: 'Wattson markers window'}));
}

async function addWattsonCpuElements(
  ctx: Trace,
  group: TrackNode,
  missingEvents: string[],
  hasCpuIdleCounters: boolean,
) {
  const warningDesc = createCpuWarnings(missingEvents, hasCpuIdleCounters);

  // CPUs estimate as part of CPU subsystem
  const estimateSuffix = `${hasCpuIdleCounters ? '' : ' crude'} estimate`;
  const schedPlugin = ctx.plugins.getPlugin(SchedPlugin);
  const schedCpus = schedPlugin.schedCpus;
  for (const cpu of schedCpus) {
    const queryKey = `cpu${cpu.ucpu}_mw`;
    const uri = `/wattson/cpu_subsystem_estimate_cpu${cpu.ucpu}`;
    ctx.tracks.registerTrack({
      uri,
      description: () => warningDesc,
      renderer: new WattsonSubsystemEstimateTrack(
        ctx,
        uri,
        queryKey,
        `CpuSubsystem`,
      ),
      tags: {
        kinds: [CPUSS_ESTIMATE_TRACK_KIND],
        wattson: `CPU${cpu.ucpu}`,
      },
    });
    group.addChildInOrder(
      new TrackNode({
        uri,
        name: `Cpu${cpu.toString()}${estimateSuffix}`,
      }),
    );
  }

  const uri = `/wattson/cpu_subsystem_estimate_dsu_scu`;
  ctx.tracks.registerTrack({
    uri,
    renderer: new WattsonSubsystemEstimateTrack(
      ctx,
      uri,
      `dsu_scu_mw`,
      `CpuSubsystem`,
    ),
    tags: {
      kinds: [CPUSS_ESTIMATE_TRACK_KIND],
      wattson: 'Dsu_Scu',
    },
  });
  group.addChildInOrder(new TrackNode({uri, name: `DSU/SCU${estimateSuffix}`}));

  // Register selection aggregators.
  // NOTE: the registration order matters because the laste two aggregators
  // depend on views created by the first two.
  ctx.selection.registerAreaSelectionTab(
    createAggregationTab(ctx, new WattsonEstimateSelectionAggregator()),
  );
  ctx.selection.registerAreaSelectionTab(
    createAggregationTab(ctx, new WattsonThreadSelectionAggregator()),
  );
  ctx.selection.registerAreaSelectionTab(
    createAggregationTab(ctx, new WattsonProcessSelectionAggregator()),
  );

  if (await isProcessMetadataPresent(ctx.engine)) {
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new WattsonPackageSelectionAggregator()),
    );
  }
}

async function isProcessMetadataPresent(engine: Engine) {
  const packageInfo = await engine.query(`
    INCLUDE PERFETTO MODULE android.process_metadata;
    SELECT COUNT(*) as count FROM android_process_metadata
    WHERE package_name IS NOT NULL
  `);
  return packageInfo.firstRow({count: NUM}).count > 0;
}

async function addWattsonGpuElements(ctx: Trace, group: TrackNode) {
  const id = `/wattson/gpu_subsystem_estimate`;
  ctx.tracks.registerTrack({
    uri: id,
    renderer: new WattsonSubsystemEstimateTrack(
      ctx,
      id,
      `gpu_mw`,
      `GpuSubsystem`,
    ),
    tags: {
      kinds: [GPUSS_ESTIMATE_TRACK_KIND],
      wattson: 'Gpu',
    },
  });
  group.addChildInOrder(new TrackNode({uri: id, name: `GPU Estimate`}));
}
