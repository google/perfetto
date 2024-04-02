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

import {FtraceExplorer} from './ftrace_explorer';
import {
  EngineProxy,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {NUM, STR} from '../../trace_processor/query_result';
import {Trash} from '../../base/disposable';
import {FtraceFilter, FtracePluginState, FtraceStat} from './common';
import {FtraceRawTrack} from './ftrace_track';

const VERSION = 1;

const DEFAULT_STATE: FtracePluginState = {
  version: VERSION,
  filter: {
    excludeList: [],
  },
};

class FtraceRawPlugin implements Plugin {
  private trash = new Trash();

  onActivate(_: PluginContext) {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const store = ctx.mountStore<FtracePluginState>((init: unknown) => {
      if (
        typeof init === 'object' &&
        init !== null &&
        'version' in init &&
        init.version === VERSION
      ) {
        return init as {} as FtracePluginState;
      } else {
        return DEFAULT_STATE;
      }
    });
    this.trash.add(store);

    const filterStore = store.createSubStore(
      ['filter'],
      (x) => x as FtraceFilter,
    );
    this.trash.add(filterStore);

    const counters = await this.getFtraceCounters(ctx.engine);

    const cpus = await this.lookupCpuCores(ctx.engine);
    for (const cpuNum of cpus) {
      const uri = `perfetto.FtraceRaw#cpu${cpuNum}`;

      ctx.registerTrack({
        uri,
        displayName: `Ftrace Track for CPU ${cpuNum}`,
        cpu: cpuNum,
        trackFactory: () => {
          return new FtraceRawTrack(ctx.engine, cpuNum, filterStore);
        },
      });
    }

    const ftraceTabUri = 'perfetto.FtraceRaw#FtraceEventsTab';

    ctx.registerTab({
      uri: ftraceTabUri,
      isEphemeral: false,
      content: {
        render: () =>
          m(FtraceExplorer, {
            counters,
            filterStore,
            engine: ctx.engine,
          }),
        getTitle: () => 'Ftrace Events',
      },
    });

    ctx.registerCommand({
      id: 'perfetto.FtraceRaw#ShowFtraceTab',
      name: 'Show Ftrace Tab',
      callback: () => {
        ctx.tabs.showTab(ftraceTabUri);
      },
    });
  }

  async onTraceUnload(): Promise<void> {
    this.trash.dispose();
  }

  private async getFtraceCounters(engine: EngineProxy): Promise<FtraceStat[]> {
    // Pull out the counts ftrace events by name
    const query = `select
          name,
          count(name) as cnt
        from ftrace_event
        group by name
        order by cnt desc`;
    const result = await engine.query(query);
    const counters: FtraceStat[] = [];
    const it = result.iter({name: STR, cnt: NUM});
    for (let row = 0; it.valid(); it.next(), row++) {
      counters.push({name: it.name, count: it.cnt});
    }
    return counters;
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
