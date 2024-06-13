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

import {FtraceExplorer, FtraceExplorerCache} from './ftrace_explorer';
import {
  Engine,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {NUM} from '../../trace_processor/query_result';
import {DisposableStack} from '../../base/disposable';
import {FtraceFilter, FtracePluginState} from './common';
import {FtraceRawTrack} from './ftrace_track';

const VERSION = 1;

const DEFAULT_STATE: FtracePluginState = {
  version: VERSION,
  filter: {
    excludeList: [],
  },
};

class FtraceRawPlugin implements Plugin {
  private trash = new DisposableStack();

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
    this.trash.use(store);

    const filterStore = store.createSubStore(
      ['filter'],
      (x) => x as FtraceFilter,
    );
    this.trash.use(filterStore);

    const cpus = await this.lookupCpuCores(ctx.engine);
    for (const cpuNum of cpus) {
      const uri = `perfetto.FtraceRaw#cpu${cpuNum}`;

      ctx.registerStaticTrack({
        uri,
        groupName: 'Ftrace Events',
        displayName: `Ftrace Track for CPU ${cpuNum}`,
        cpu: cpuNum,
        trackFactory: () => {
          return new FtraceRawTrack(ctx.engine, cpuNum, filterStore);
        },
      });
    }

    const cache: FtraceExplorerCache = {
      state: 'blank',
      counters: [],
    };

    const ftraceTabUri = 'perfetto.FtraceRaw#FtraceEventsTab';

    ctx.registerTab({
      uri: ftraceTabUri,
      isEphemeral: false,
      content: {
        render: () =>
          m(FtraceExplorer, {
            filterStore,
            cache,
            engine: ctx.engine,
          }),
        getTitle: () => 'Ftrace Events',
      },
    });

    ctx.registerCommand({
      id: 'perfetto.FtraceRaw#ShowFtraceTab',
      name: 'Show ftrace tab',
      callback: () => {
        ctx.tabs.showTab(ftraceTabUri);
      },
    });
  }

  async onTraceUnload(): Promise<void> {
    this.trash.dispose();
  }

  private async lookupCpuCores(engine: Engine): Promise<number[]> {
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
