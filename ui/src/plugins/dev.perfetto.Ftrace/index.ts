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

import './styles.scss';
import m from 'mithril';

import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {Cpu} from '../../components/cpu';
import {getMachineCount} from '../../public/utils';
import {
  type FtraceFilter,
  type FtracePluginState as FtraceFilters,
  FTRACE_RAW_TRACK_KIND,
} from './common';
import {FtraceExplorer, type FtraceExplorerCache} from './ftrace_explorer';
import {createFtraceTrack} from './ftrace_track';

const VERSION = 2;

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

    const ftraceTabUri = 'perfetto.FtraceRaw#FtraceEventsTab';

    let hasExpandedOnce = false;

    const numMachines = await getMachineCount(ctx.engine);
    const cpus = await getFtraceCpus(ctx, numMachines);
    const group = new TrackNode({
      name: 'Ftrace Events',
      sortOrder: -5,
      isSummary: true,
      onExpand: () => {
        if (!hasExpandedOnce) {
          hasExpandedOnce = true;
          ctx.tabs.showTab(ftraceTabUri);
        }
      },
    });

    for (const cpu of cpus) {
      const uri = `/ftrace/cpu${cpu.ucpu}`;

      ctx.tracks.registerTrack({
        uri,
        description: `Ftrace events for CPU ${cpu.toString()}`,
        tags: {
          cpu: cpu.cpu,
          ucpu: cpu.ucpu,
          kinds: [FTRACE_RAW_TRACK_KIND],
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

    // The event-name filter is shared (and persisted) across both the
    // standalone tab and the area-selection tab.
    const onExcludeListChange = (excludeList: ReadonlyArray<string>) =>
      filterStore.edit((draft) => {
        draft.excludeList = Array.from(excludeList);
      });

    const allUcpus = cpus.map((c) => c.ucpu);

    ctx.tabs.registerTab({
      uri: ftraceTabUri,
      isEphemeral: false,
      content: {
        render: () =>
          m(FtraceExplorer, {
            trace: ctx,
            cache,
            cpus,
            excludeList: filterStore.state.excludeList,
            onExcludeListChange,
            // The standalone tab exposes a persisted cpu inclusion filter over
            // all cpus. Undefined persisted state means "show all".
            cpuFilter: {
              kind: 'selectable',
              show: filterStore.state.visibleCpus ?? allUcpus,
              onChange: (show) =>
                filterStore.edit((draft) => {
                  draft.visibleCpus = Array.from(show);
                }),
            },
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

    // Also use the ftrace explorer for area selections, as a child of the
    // selection tab. It shares the (persisted) event-name filter with the
    // standalone tab, but takes its CPU list from the selected ftrace tracks.
    ctx.selection.registerAreaSelectionTab({
      id: 'ftrace_area_selection',
      name: 'Ftrace Events',
      priority: 100,
      render(selection) {
        const selectedUcpus = selection.tracks
          .filter((t) => t.tags?.kinds?.includes(FTRACE_RAW_TRACK_KIND))
          .map((t) => t.tags?.ucpu)
          .filter((ucpu): ucpu is number => typeof ucpu === 'number');
        if (selectedUcpus.length === 0) return undefined;

        return {
          isLoading: false,
          content: m(FtraceExplorer, {
            trace: ctx,
            cache,
            cpus,
            bounds: {start: selection.start, end: selection.end},
            excludeList: filterStore.state.excludeList,
            onExcludeListChange,
            cpuFilter: {kind: 'fixed', show: selectedUcpus},
          }),
        };
      },
    });
  }
}

/**
 * Get the list of unique cpus in the ftrace_event table.
 */
async function getFtraceCpus(ctx: Trace, numMachines: number): Promise<Cpu[]> {
  // Compute the DISTINCT set of cpus first (a full scan of ftrace_event, but
  // only touching the ucpu column) and then join the handful of resulting rows
  // against cpu/machine. Joining before the DISTINCT would run the joins for
  // every ftrace event.
  const queryRes = await ctx.engine.query(`
    SELECT
      ucpu,
      cpu.machine_id AS machine_id,
      cpu.cpu AS cpu,
      machine.name AS machine_name,
      machine.label_index AS machine_label_index
    FROM (SELECT DISTINCT ucpu FROM ftrace_event)
    JOIN cpu USING (ucpu)
    LEFT JOIN machine ON machine.id = cpu.machine_id
    ORDER BY ucpu
  `);

  const ucpus: Cpu[] = [];
  for (
    const it = queryRes.iter({
      ucpu: NUM,
      machine_id: NUM,
      cpu: NUM,
      machine_name: STR_NULL,
      machine_label_index: NUM_NULL,
    });
    it.valid();
    it.next()
  ) {
    ucpus.push(
      new Cpu(
        it.ucpu,
        it.cpu,
        it.machine_id,
        it.machine_name ?? undefined,
        it.machine_label_index ?? undefined,
        numMachines,
      ),
    );
  }

  return ucpus;
}
