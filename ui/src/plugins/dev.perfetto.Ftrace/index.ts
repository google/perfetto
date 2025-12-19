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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {NUM} from '../../trace_processor/query_result';
import {Cpu} from '../../components/cpu';
import {FtraceFilter, FtracePluginState as FtraceFilters} from './common';
import {FtraceExplorer, FtraceExplorerCache} from './ftrace_explorer';
import {createFtraceTrack} from './ftrace_track';

const VERSION = 1;

const DEFAULT_STATE: FtraceFilters = {
  version: VERSION,
  filter: {
    excludeList: [],
  },
};

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Ftrace';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const store = ctx.mountStore<FtraceFilters>(
      'dev.perfetto.FtraceFilters',
      (init: unknown) => {
        if (
          typeof init === 'object' &&
          init !== null &&
          'version' in init &&
          init.version === VERSION
        ) {
          return init as {} as FtraceFilters;
        } else {
          return DEFAULT_STATE;
        }
      },
    );

    const filterStore = store.createSubStore(
      ['filter'],
      (x) => x as FtraceFilter,
    );

    const cpus = await getFtraceCpus(ctx);
    const group = new TrackNode({
      name: 'Ftrace Events',
      sortOrder: -5,
      isSummary: true,
    });

    for (const cpu of cpus) {
      const uri = `/ftrace/cpu${cpu.ucpu}`;

      ctx.tracks.registerTrack({
        uri,
        description: `Ftrace events for CPU ${cpu.toString()}`,
        tags: {
          cpu: cpu.cpu,
        },
        renderer: createFtraceTrack(ctx, uri, cpu.ucpu, filterStore),
      });

      const track = new TrackNode({
        uri,
        name: `Ftrace Track for CPU ${cpu.toString()}`,
      });
      group.addChildInOrder(track);
    }

    if (group.children.length) {
      ctx.defaultWorkspace.addChildInOrder(group);
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
      id: 'dev.perfetto.ShowFtraceTab',
      name: 'Show ftrace tab',
      callback: () => {
        ctx.tabs.showTab(ftraceTabUri);
      },
    });
  }
}

/**
 * Get the list of unique cpus in the ftrace_event table.
 */
async function getFtraceCpus(ctx: Trace): Promise<Cpu[]> {
  const queryRes = await ctx.engine.query(`
    SELECT DISTINCT
      ucpu,
      IFNULL(cpu.machine_id, 0) AS machine_id,
      cpu.cpu AS cpu
    FROM ftrace_event
    JOIN cpu USING (ucpu)
    ORDER BY ucpu
  `);

  const ucpus: Cpu[] = [];
  for (
    const it = queryRes.iter({ucpu: NUM, machine_id: NUM, cpu: NUM});
    it.valid();
    it.next()
  ) {
    ucpus.push(new Cpu(it.ucpu, it.cpu, it.machine_id));
  }

  return ucpus;
}
