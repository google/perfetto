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

import {assertUnreachable} from '../../base/logging';
import {Cpu} from '../../base/multi_machine_trace';
import {Time} from '../../base/time';
import {materialColorScheme} from '../../components/colorizer';
import {renderArguments} from '../../components/details/slice_args';
import {Arg, ArgValue, ArgValueType} from '../../components/sql_utils/args';
import {asArgId} from '../../components/sql_utils/core_types';
import {DatasetSliceTrack} from '../../components/tracks/dataset_slice_track';
import {Timestamp} from '../../components/widgets/timestamp';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {FtraceFilter, FtracePluginState} from './common';
import {FtraceExplorer, FtraceExplorerCache} from './ftrace_explorer';

const VERSION = 1;
const MARKER_WIDTH_PX = 8;
const FTRACE_EXPLORER_TAB_URI = 'perfetto.FtraceRaw#FtraceEventsTab';

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
        track: new DatasetSliceTrack({
          trace: ctx,
          uri,
          dataset: () =>
            // This is called every cycle to get live updates from the plugin.
            // If the dataset evaluates to a different query then the base
            // data structures are re-evaluated.
            new SourceDataset({
              src: `
                SELECT *
                FROM ftrace_event
                WHERE
                  name NOT IN (${filterStore.state.excludeList.map((x) => `'${x}'`).join(', ')})
              `,
              schema: {
                id: NUM,
                ts: LONG,
                name: STR,
                cpu: NUM,
              },
              filter: {
                col: 'ucpu',
                eq: cpu.ucpu,
              },
            }),
          colorizer: (row) => materialColorScheme(row.name),
          instantStyle: {
            width: MARKER_WIDTH_PX,
            render: (ctx, r) => ctx.fillRect(r.x, r.y, r.width, r.height),
          },
          tooltip: (row) => row.row.name,
          detailsPanel: (row) => {
            return new FtraceEventDetailsPanel(ctx, row);
          },
        }),
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

    ctx.tabs.registerTab({
      uri: FTRACE_EXPLORER_TAB_URI,
      isEphemeral: false,
      content: {
        render: () =>
          m(FtraceExplorer, {
            filterStore,
            cache,
            trace: ctx,
          }),
        getTitle: () => 'Ftrace Explorer',
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.FtraceRaw#ShowFtraceTab',
      name: 'Show ftrace tab',
      callback: () => {
        ctx.tabs.showTab(FTRACE_EXPLORER_TAB_URI);
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

class FtraceEventDetailsPanel {
  private args?: ReadonlyArray<Arg>;

  constructor(
    readonly trace: Trace,
    readonly row: Readonly<{
      id: number;
      ts: bigint;
      name: string;
      cpu: number;
    }>,
  ) {}

  async load() {
    await this.loadArgs();
  }

  render() {
    return m(
      DetailsShell,
      {
        title: `Ftrace Event`,
        description: this.row.name,
      },
      m(
        GridLayout,
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Details'},
            m(
              Tree,
              m(TreeNode, {
                left: 'ID',
                right: this.row.id,
              }),
              m(TreeNode, {
                left: 'Name',
                right: this.row.name,
              }),
              m(TreeNode, {
                left: 'Timestamp',
                right: m(Timestamp, {ts: Time.fromRaw(this.row.ts)}),
              }),
              m(TreeNode, {
                left: 'CPU',
                right: this.row.cpu,
              }),
            ),
          ),
        ),
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Arguments'},
            m(Tree, this.args && renderArguments(this.trace, this.args)),
          ),
        ),
      ),
    );
  }

  private async loadArgs() {
    const queryRes = await this.trace.engine.query(`
      SELECT
        args.id as id,
        flat_key as flatKey,
        key,
        int_value as intValue,
        string_value as stringValue,
        real_value as realValue,
        value_type as valueType,
        display_value as displayValue
      FROM ftrace_event
      JOIN args USING(arg_set_id)
      WHERE ftrace_event.id = ${this.row.id}
    `);

    const it = queryRes.iter({
      id: NUM,
      flatKey: STR,
      key: STR,
      intValue: LONG_NULL,
      stringValue: STR_NULL,
      realValue: NUM_NULL,
      valueType: STR,
      displayValue: STR_NULL,
    });

    const args: Arg[] = [];
    for (; it.valid(); it.next()) {
      const value = parseArgValue(it);
      args.push({
        id: asArgId(it.id),
        flatKey: it.flatKey,
        key: it.key,
        value,
        displayValue: it.displayValue ?? 'NULL',
      });
    }
    this.args = args;
  }
}

function parseArgValue(it: {
  valueType: string;
  intValue: bigint | null;
  stringValue: string | null;
  realValue: number | null;
}): ArgValue {
  const valueType = it.valueType as ArgValueType;
  switch (valueType) {
    case 'int':
    case 'uint':
      return it.intValue;
    case 'pointer':
      return it.intValue === null ? null : `0x${it.intValue.toString(16)}`;
    case 'string':
      return it.stringValue;
    case 'bool':
      return Boolean(it.intValue);
    case 'real':
      return it.realValue;
    case 'null':
      return null;
    default:
      assertUnreachable(valueType);
  }
}
