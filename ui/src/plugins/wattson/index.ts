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
} from '../../frontend/base_counter_track';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {CPUSS_ESTIMATE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {WattsonEstimateSelectionAggregator} from './estimate_aggregator';
import {WattsonPackageSelectionAggregator} from './package_aggregator';
import {WattsonProcessSelectionAggregator} from './process_aggregator';
import {WattsonThreadSelectionAggregator} from './thread_aggregator';
import {Engine} from '../../trace_processor/engine';
import {NUM} from '../../trace_processor/query_result';

class Wattson implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    // Short circuit if Wattson is not supported for this Perfetto trace
    if (!(await hasWattsonSupport(ctx.engine))) return;

    ctx.engine.query(`INCLUDE PERFETTO MODULE wattson.curves.ungrouped;`);

    const group = new TrackNode({title: 'Wattson', isSummary: true});
    ctx.workspace.addChildInOrder(group);

    // CPUs estimate as part of CPU subsystem
    const cpus = ctx.traceInfo.cpus;
    for (const cpu of cpus) {
      const queryKey = `cpu${cpu}_curve`;
      const uri = `/wattson/cpu_subsystem_estimate_cpu${cpu}`;
      const title = `Cpu${cpu} Estimate`;
      ctx.tracks.registerTrack({
        uri,
        title,
        track: new CpuSubsystemEstimateTrack(ctx, uri, queryKey),
        tags: {
          kind: CPUSS_ESTIMATE_TRACK_KIND,
          wattson: `CPU${cpu}`,
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
      track: new CpuSubsystemEstimateTrack(ctx, uri, `dsu_scu`),
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
    ctx.selection.registerAreaSelectionAggreagtor(
      new WattsonEstimateSelectionAggregator(),
    );
    ctx.selection.registerAreaSelectionAggreagtor(
      new WattsonThreadSelectionAggregator(),
    );
    ctx.selection.registerAreaSelectionAggreagtor(
      new WattsonPackageSelectionAggregator(),
    );
    ctx.selection.registerAreaSelectionAggreagtor(
      new WattsonProcessSelectionAggregator(),
    );
  }
}

class CpuSubsystemEstimateTrack extends BaseCounterTrack {
  readonly queryKey: string;

  constructor(trace: Trace, uri: string, queryKey: string) {
    super({
      trace,
      uri,
    });
    this.queryKey = queryKey;
  }

  protected getDefaultCounterOptions(): CounterOptions {
    const options = super.getDefaultCounterOptions();
    options.yRangeSharingKey = `CpuSubsystem`;
    options.unit = `mW`;
    return options;
  }

  getSqlSource() {
    if (this.queryKey.startsWith(`cpu`)) {
      return `select ts, ${this.queryKey} as value from _system_state_curves`;
    } else {
      return `
        select
          ts,
          -- L3 values are scaled by 1000 because it's divided by ns and L3 LUTs
          -- are scaled by 10^6. This brings to same units as static_curve (mW)
          ((IFNULL(l3_hit_value, 0) + IFNULL(l3_miss_value, 0)) * 1000 / dur)
            + static_curve  as value
        from _system_state_curves
      `;
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: `org.kernel.Wattson`,
  plugin: Wattson,
};

async function hasWattsonSupport(engine: Engine): Promise<boolean> {
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
