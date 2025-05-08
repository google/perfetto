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

import {Cpu} from '../../base/multi_machine_trace';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {NUM} from '../../trace_processor/query_result';
import {FtraceFilter, FtracePluginState} from './common';
import {FtraceExplorer, FtraceExplorerCache} from './ftrace_explorer';
import {FtraceRawTrack} from './ftrace_track';

const VERSION = 1;

const DEFAULT_STATE: FtracePluginState = {
  version: VERSION,
  filter: {
    excludeList: [],
  },
};

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Ftrace';
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
    ctx.trash.use(store);

    const filterStore = store.createSubStore(
      ['filter'],
      (x) => x as FtraceFilter,
    );
    ctx.trash.use(filterStore);

    const cpus = await this.lookupCpuCores(ctx);
    const group = new TrackNode({
      title: 'Ftrace Events',
      sortOrder: -5,
      isSummary: true,
    });

    for (const cpu of cpus) {
      const uri = `/ftrace/cpu${cpu.ucpu}`;
      const title = `Ftrace Track for CPU ${cpu.toString()}`;

      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          cpu: cpu.cpu,
          groupName: 'Ftrace Events',
        },
        track: new FtraceRawTrack(ctx.engine, cpu.ucpu, filterStore),
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

  private async lookupCpuCores(ctx: Trace): Promise<Cpu[]> {
    // ctx.traceInfo.cpus contains all cpus seen from all events. Filter the set
    // if it's seen in ftrace_event.
    const queryRes = await ctx.engine.query(
      `select distinct ucpu from ftrace_event order by ucpu;`,
    );
    const ucpus = new Set<number>();
    for (const it = queryRes.iter({ucpu: NUM}); it.valid(); it.next()) {
      ucpus.add(it.ucpu);
    }

    const cpuCores = ctx.traceInfo.cpus.filter((cpu) => ucpus.has(cpu.ucpu));
    return cpuCores;
  }
}
