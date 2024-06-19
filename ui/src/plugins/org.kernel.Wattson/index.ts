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
import {uuidv4Sql} from '../../base/uuid';
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

class Wattson implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.engine.query(`INCLUDE PERFETTO MODULE wattson.curves.ungrouped;`);

    const cpus = globals.traceContext.cpus;
    for (const cpu of cpus) {
      ctx.registerStaticTrack({
        uri: `perfetto.CpuEstimate#CPU${cpu}`,
        displayName: `Cpu${cpu} Estimate`,
        kind: `CpuEstimateTrack`,
        trackFactory: () => new CpuEstimateTrack(ctx.engine, cpu),
        groupName: `Wattson`,
      });
    }
  }
}

class CpuEstimateTrack extends BaseCounterTrack {
  protected engine: Engine;
  private cpu: number;

  constructor(engine: Engine, cpu: number) {
    super({
      engine: engine,
      trackKey: uuidv4Sql(),
    });
    this.engine = engine;
    this.cpu = cpu;
  }

  protected getDefaultCounterOptions(): CounterOptions {
    const options = super.getDefaultCounterOptions();
    options.unit = `mW`;
    return options;
  }

  getSqlSource() {
    return `
      select
        ts,
        cpu${this.cpu}_curve as value
      from _system_state_curves
    `;
  }
}

export const plugin: PluginDescriptor = {
  pluginId: `org.kernel.Wattson`,
  plugin: Wattson,
};
