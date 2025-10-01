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

import m from 'mithril';

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
import {
  LONG,
  LONG_NULL,
  NUM,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {WattsonEstimateSelectionAggregator} from './estimate_aggregator';
import {WattsonPackageSelectionAggregator} from './package_aggregator';
import {WattsonProcessSelectionAggregator} from './process_aggregator';
import {WattsonThreadSelectionAggregator} from './thread_aggregator';
import {
  CPUSS_ESTIMATE_TRACK_KIND,
  GPUSS_ESTIMATE_TRACK_KIND,
} from './track_kinds';
import SchedPlugin from '../dev.perfetto.Sched';

export default class implements PerfettoPlugin {
  static readonly id = `org.kernel.Wattson`;
  static readonly dependencies = [SchedPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const markersSupported = await hasWattsonMarkersSupport(ctx.engine);
    const cpuSupported = await hasWattsonCpuSupport(ctx.engine);
    const gpuSupported = await hasWattsonGpuSupport(ctx.engine);
    const missingCpuConfigs = await hasWattsonSufficientCPUConfigs(ctx.engine);

    // Short circuit if Wattson is not supported for this Perfetto trace
    if (!(markersSupported || cpuSupported || gpuSupported)) return;

    const group = new TrackNode({name: 'Wattson', isSummary: true});
    ctx.workspace.addChildInOrder(group);

    if (markersSupported) {
      await addWattsonMarkersElements(ctx, group);
    }
    if (cpuSupported) {
      await addWattsonCpuElements(ctx, group, missingCpuConfigs);
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

// Walk through user's Perfetto Trace Configs and check
// against bare minimum configs that makes Wattson work.
// Add the missing ones to missingEvents, display in UI.
async function hasWattsonSufficientCPUConfigs(
  engine: Engine,
): Promise<string[]> {
  const requiredFtraceEvents: string[] = [
    'power/cpu_frequency',
    'power/cpu_idle',
  ];

  const dsuDependencyQuery = await engine.query(
    `
    INCLUDE PERFETTO MODULE wattson.curves.utils;
    SELECT count(*) AS count FROM _cpu_w_dsu_dependency;
    `,
  );

  if (dsuDependencyQuery.firstRow({count: NUM}).count > 0) {
    requiredFtraceEvents.push('devfreq/devfreq_frequency');
  }

  const missingEvents: string[] = [];
  const query = `
    SELECT str_value
    FROM metadata
    WHERE name = 'trace_config_pbtxt';
    `;

  const result = await engine.query(query);
  const row = result.maybeFirstRow({str_value: STR_NULL});
  const traceConfig = row?.str_value || '';

  for (const event of requiredFtraceEvents) {
    const eventPattern = new RegExp(`ftrace_events:\\s*"${event}"`);
    if (!eventPattern.test(traceConfig)) {
      missingEvents.push(event);
    }
  }

  return missingEvents;
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
) {
  const warningDesc =
    missingEvents.length > 0
      ? m(
          '.pf-wattson-warning',
          'Perfetto trace configuration is missing below trace_events for Wattson to work:',
          m(
            '.pf-wattson-warning__list',
            missingEvents.map((event) => m('li', event)),
          ),
        )
      : undefined;

  // CPUs estimate as part of CPU subsystem
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
        name: `Cpu${cpu.toString()} Estimate`,
      }),
    );
  }

  const uri = `/wattson/cpu_subsystem_estimate_dsu_scu`;
  const title = `DSU/SCU Estimate`;
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
  group.addChildInOrder(new TrackNode({uri, name: title}));

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
