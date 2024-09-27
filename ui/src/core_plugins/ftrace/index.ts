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
import {Engine} from '../../trace_processor/engine';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {NUM} from '../../trace_processor/query_result';
import {FtraceFilter, FtracePluginState} from './common';
import {FtraceRawTrack} from './ftrace_track';
import {DisposableStack} from '../../base/disposable_stack';
import {TrackNode} from '../../public/workspace';

const VERSION = 1;

const DEFAULT_STATE: FtracePluginState = {
  version: VERSION,
  filter: {
    excludeList: [],
  },
};

class FtraceRawPlugin implements PerfettoPlugin {
  private trash = new DisposableStack();

  async onTraceLoad(ctx: Trace): Promise<void> {
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
    const group = new TrackNode({
      title: 'Ftrace Events',
      sortOrder: -5,
      isSummary: true,
    });

    for (const cpuNum of cpus) {
      const uri = `/ftrace/cpu${cpuNum}`;
      const title = `Ftrace Track for CPU ${cpuNum}`;

      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          cpu: cpuNum,
          groupName: 'Ftrace Events',
        },
        track: new FtraceRawTrack(ctx.engine, cpuNum, filterStore),
      });

      const track = new TrackNode({uri, title});
      group.addChildInOrder(track);
    }

    if (group.children.length) {
      ctx.workspace.addChildInOrder(group);
    }

    const cache: FtraceExplorerCache = {
      state: 'blank',
      counters: [],
    };

    const ftraceTabUri = 'perfetto.FtraceRaw#FtraceEventsTab';

    ctx.tabs.registerTab({
      uri: ftraceTabUri,
      isEphemeral: false,
      content: {
        render: () =>
          m(FtraceExplorer, {
            filterStore,
            cache,
            trace: ctx,
          }),
        getTitle: () => 'Ftrace Events',
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.FtraceRaw#ShowFtraceTab',
      name: 'Show ftrace tab',
      callback: () => {
        ctx.tabs.showTab(ftraceTabUri);
      },
    });
  }

  async onTraceUnload(): Promise<void> {
    this.trash[Symbol.dispose]();
  }

  private async lookupCpuCores(engine: Engine): Promise<number[]> {
    const query = 'select distinct cpu from ftrace_event order by cpu';

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
