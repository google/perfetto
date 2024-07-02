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

import m from 'mithril';

import {AsyncLimiter} from '../../base/async_limiter';
import {assertExists} from '../../base/logging';
import {Monitor} from '../../base/monitor';
import {time} from '../../base/time';
import {featureFlags} from '../../core/feature_flags';
import {LegacyFlamegraphCache} from '../../core/legacy_flamegraph_cache';
import {
  HeapProfileSelection,
  LegacySelection,
  ProfileType,
} from '../../core/selection_manager';
import {
  LegacyFlamegraphDetailsPanel,
  profileType,
} from '../../frontend/legacy_flamegraph_panel';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {
  Engine,
  LegacyDetailsPanel,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {computeFlamegraphTree} from '../../core/flamegraph_query_utils';
import {NUM} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {
  Flamegraph,
  FlamegraphFilters,
  FlamegraphQueryData,
} from '../../widgets/flamegraph';

import {HeapProfileTrack} from './heap_profile_track';

export const HEAP_PROFILE_TRACK_KIND = 'HeapProfileTrack';

class HeapProfilePlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
      select distinct(upid) from heap_profile_allocation
      union
      select distinct(upid) from heap_graph_object
    `);
    for (const it = result.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      ctx.registerTrack({
        uri: `perfetto.HeapProfile#${upid}`,
        displayName: 'Heap Profile',
        kind: HEAP_PROFILE_TRACK_KIND,
        upid,
        trackFactory: ({trackKey}) => {
          return new HeapProfileTrack(
            {
              engine: ctx.engine,
              trackKey,
            },
            upid,
          );
        },
      });
    }
    ctx.registerDetailsPanel(new HeapProfileFlamegraphDetailsPanel(ctx.engine));
  }
}

const FLAMEGRAPH_METRICS = [
  {
    name: 'Object Size',
    unit: 'B',
    dependencySql: `
      include perfetto module android.memory.heap_graph.class_tree;
    `,
    sqlFn: (ts: time, upid: number) => `
      select id, parent_id as parentId, name, self_size as value
      from _heap_graph_class_tree
      where graph_sample_ts = ${ts} and upid = ${upid}
    `,
  },
  {
    name: 'Object Count',
    unit: '',
    dependencySql: `
      include perfetto module android.memory.heap_graph.class_tree;
    `,
    sqlFn: (ts: time, upid: number) => `
      select id, parent_id as parentId, name, self_count as value
      from _heap_graph_class_tree
      where graph_sample_ts = ${ts} and upid = ${upid}
    `,
  },
  {
    name: 'Dominated Object Size',
    unit: 'B',
    dependencySql: `
      include perfetto module android.memory.heap_graph.dominator_class_tree;
    `,
    sqlFn: (ts: time, upid: number) => `
      select id, parent_id as parentId, name, self_size as value
      from _heap_graph_dominator_class_tree
      where graph_sample_ts = ${ts} and upid = ${upid}
    `,
  },
  {
    name: 'Dominated Object Count',
    unit: '',
    dependencySql: `
      include perfetto module android.memory.heap_graph.dominator_class_tree;
    `,
    sqlFn: (ts: time, upid: number) => `
      select id, parent_id as parentId, name, self_count as value
      from _heap_graph_dominator_class_tree
      where graph_sample_ts = ${ts} and upid = ${upid}
    `,
  },
];
const DEFAULT_SELECTED_METRIC_NAME = 'Object Size';

const USE_NEW_FLAMEGRAPH_IMPL = featureFlags.register({
  id: 'useNewFlamegraphImpl',
  name: 'Use new flamegraph implementation',
  description: 'Use new flamgraph implementation in details panels.',
  defaultValue: true,
});

class HeapProfileFlamegraphDetailsPanel implements LegacyDetailsPanel {
  private sel?: HeapProfileSelection;
  private selMonitor = new Monitor([
    () => this.sel?.ts,
    () => this.sel?.upid,
    () => this.sel?.type,
  ]);
  private cache = new LegacyFlamegraphCache('heap_profile');
  private queryLimiter = new AsyncLimiter();

  private selectedMetricName = DEFAULT_SELECTED_METRIC_NAME;
  private data?: FlamegraphQueryData;
  private filters: FlamegraphFilters = {
    showStack: [],
    hideStack: [],
    showFrame: [],
    hideFrame: [],
  };

  constructor(private engine: Engine) {}

  render(sel: LegacySelection) {
    if (sel.kind !== 'HEAP_PROFILE') {
      this.sel = undefined;
      return undefined;
    }
    if (
      sel.type !== ProfileType.JAVA_HEAP_GRAPH &&
      !USE_NEW_FLAMEGRAPH_IMPL.get()
    ) {
      this.sel = undefined;
      return m(LegacyFlamegraphDetailsPanel, {
        cache: this.cache,
        selection: {
          profileType: profileType(sel.type),
          start: sel.ts,
          end: sel.ts,
          upids: [sel.upid],
        },
      });
    }

    this.sel = sel;
    if (this.selMonitor.ifStateChanged()) {
      this.selectedMetricName = DEFAULT_SELECTED_METRIC_NAME;
      this.data = undefined;
      this.fetchData();
    }
    return m(
      '.flamegraph-profile',
      m(
        DetailsShell,
        {
          fillParent: true,
          title: m('div.title', 'Java Heap Graph'),
          description: [],
          buttons: [
            m(
              'div.time',
              `Snapshot time: `,
              m(Timestamp, {
                ts: sel.ts,
              }),
            ),
          ],
        },
        m(Flamegraph, {
          metrics: FLAMEGRAPH_METRICS,
          selectedMetricName: this.selectedMetricName,
          data: this.data,
          onMetricChange: (name) => {
            this.selectedMetricName = name;
            this.data = undefined;
            this.fetchData();
          },
          onFiltersChanged: (filters) => {
            this.filters = filters;
            this.data = undefined;
            this.fetchData();
          },
        }),
      ),
    );
  }

  private async fetchData() {
    if (this.sel === undefined) {
      return;
    }
    const {ts, upid} = this.sel;
    const selectedMetricName = this.selectedMetricName;
    const filters = this.filters;
    this.queryLimiter.schedule(async () => {
      const {sqlFn, dependencySql} = assertExists(
        FLAMEGRAPH_METRICS.find((metric) => metric.name === selectedMetricName),
      );
      const sql = sqlFn(ts, upid);
      this.data = await computeFlamegraphTree(
        this.engine,
        dependencySql,
        sql,
        filters,
      );
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.HeapProfile',
  plugin: HeapProfilePlugin,
};
