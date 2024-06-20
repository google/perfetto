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

import {globals} from '../../frontend/globals';
import {
  BaseCounterTrack,
  CounterOptions,
} from '../../frontend/base_counter_track';
import {
  Engine,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {NUM} from '../../trace_processor/query_result';

class Wattson implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.engine.query(`INCLUDE PERFETTO MODULE wattson.curves.ungrouped;`);

    // CPUs estimate as part of CPU subsystem
    const cpus = globals.traceContext.cpus;
    for (const cpu of cpus) {
      const queryKey = `cpu${cpu}_curve`;
      ctx.registerStaticTrack({
        uri: `perfetto.CpuSubsystemEstimate#CPU${cpu}`,
        displayName: `Cpu${cpu} Estimate`,
        kind: `CpuEstimateTrack`,
        trackFactory: ({trackKey}) =>
          new CpuSubsystemEstimateTrack(ctx.engine, trackKey, queryKey),
        groupName: `Wattson`,
      });
    }
    ctx.registerStaticTrack({
      uri: `perfetto.CpuSubsystemEstimate#Static`,
      displayName: `Static Estimate`,
      kind: `CpuEstimateTrack`,
      trackFactory: ({trackKey}) =>
        new CpuSubsystemEstimateTrack(ctx.engine, trackKey, `static_curve`),
      groupName: `Wattson`,
    });

    // Cache estimates for remainder of CPU subsystem
    const L3RowCount = await ctx.engine.query(`
        SELECT
          COUNT(*) as numRows
        FROM _system_state_curves
        WHERE l3_hit_value is NOT NULL AND l3_hit_value != 0
    `);
    const numL3Rows = L3RowCount.firstRow({numRows: NUM}).numRows;

    if (numL3Rows > 0) {
      const queryKeys: string[] = [`l3_hit_value`, `l3_miss_value`];
      for (const queryKey of queryKeys) {
        const keyName = queryKey.replace(`_value`, ``).replace(`l3`, `L3`);
        ctx.registerStaticTrack({
          uri: `perfetto.CpuSubsystemEstimate#${keyName}`,
          displayName: `${keyName} Estimate`,
          kind: `CacheEstimateTrack`,
          trackFactory: ({trackKey}) =>
            new CpuSubsystemEstimateTrack(ctx.engine, trackKey, queryKey),
          groupName: `Wattson`,
        });
      }
    }
  }
}

class CpuSubsystemEstimateTrack extends BaseCounterTrack {
  readonly engine: Engine;
  readonly queryKey: string;

  constructor(engine: Engine, trackKey: string, queryKey: string) {
    super({
      engine: engine,
      trackKey: trackKey,
    });
    this.engine = engine;
    this.queryKey = queryKey;
  }

  protected getDefaultCounterOptions(): CounterOptions {
    const options = super.getDefaultCounterOptions();
    options.unit = `mW`;
    return options;
  }

  getSqlSource() {
    const isL3 = this.queryKey.startsWith(`l3`);
    return isL3
      ? `
      select
        ts,
        -- scale by 1000 because dividing by ns and LUTs are scaled by 10^6
        ${this.queryKey} * 1000 / dur as value
      from _system_state_curves
    `
      : `
      select
        ts,
        ${this.queryKey} as value
      from _system_state_curves
    `;
  }
}

export const plugin: PluginDescriptor = {
  pluginId: `org.kernel.Wattson`,
  plugin: Wattson,
};
