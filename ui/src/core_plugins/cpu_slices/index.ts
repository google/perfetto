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

import {CPU_SLICE_TRACK_KIND} from '../../public';
import {SliceDetailsPanel} from '../../frontend/slice_details_panel';
import {
  Engine,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {CpuSliceTrack} from './cpu_slice_track';

class CpuSlices implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const cpus = ctx.trace.cpus;
    const cpuToSize = await this.guessCpuSizes(ctx.engine);

    for (const cpu of cpus) {
      const size = cpuToSize.get(cpu);
      const uri = `perfetto.CpuSlices#cpu${cpu}`;
      const name = size === undefined ? `Cpu ${cpu}` : `Cpu ${cpu} (${size})`;
      ctx.registerTrack({
        uri,
        displayName: name,
        kind: CPU_SLICE_TRACK_KIND,
        cpu,
        trackFactory: ({trackKey}) => {
          return new CpuSliceTrack(ctx.engine, trackKey, cpu);
        },
      });
    }

    ctx.registerDetailsPanel({
      render: (sel) => {
        if (sel.kind === 'SCHED_SLICE') {
          return m(SliceDetailsPanel);
        }
      },
    });
  }

  async guessCpuSizes(engine: Engine): Promise<Map<number, string>> {
    const cpuToSize = new Map<number, string>();
    await engine.query(`
      include perfetto module viz.core_type;
    `);
    const result = await engine.query(`
      select cpu, _guess_core_type(cpu) as size
      from cpu_counter_track
      join _counter_track_summary using (id);
    `);

    const it = result.iter({
      cpu: NUM,
      size: STR_NULL,
    });

    for (; it.valid(); it.next()) {
      const size = it.size;
      if (size !== null) {
        cpuToSize.set(it.cpu, size);
      }
    }

    return cpuToSize;
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CpuSlices',
  plugin: CpuSlices,
};
