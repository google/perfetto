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

import {assertExists} from '../../base/logging';
import {Monitor} from '../../base/monitor';
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
import {NUM} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';

import {HeapProfileTrack} from './heap_profile_track';
import {
  QueryFlamegraph,
  QueryFlamegraphAttrs,
  USE_NEW_FLAMEGRAPH_IMPL,
  metricsFromTableOrSubquery,
} from '../../core/query_flamegraph';

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

class HeapProfileFlamegraphDetailsPanel implements LegacyDetailsPanel {
  private sel?: HeapProfileSelection;
  private selMonitor = new Monitor([
    () => this.sel?.ts,
    () => this.sel?.upid,
    () => this.sel?.type,
  ]);
  private flamegraphAttrs?: QueryFlamegraphAttrs;
  private cache = new LegacyFlamegraphCache('heap_profile');

  constructor(private engine: Engine) {}

  render(sel: LegacySelection) {
    if (sel.kind !== 'HEAP_PROFILE') {
      this.sel = undefined;
      return undefined;
    }
    if (
      sel.type !== ProfileType.JAVA_HEAP_GRAPH ||
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

    const {ts, upid} = sel;
    this.sel = sel;
    if (this.selMonitor.ifStateChanged()) {
      this.flamegraphAttrs = {
        engine: this.engine,
        metrics: [
          ...metricsFromTableOrSubquery(
            `
              (
                select id, parent_id as parentId, name, self_size, self_count
                from _heap_graph_class_tree
                where graph_sample_ts = ${ts} and upid = ${upid}
              )
            `,
            [
              {
                name: 'Object Size',
                unit: 'B',
                columnName: 'self_size',
              },
              {
                name: 'Object Size',
                unit: '',
                columnName: 'self_count',
              },
            ],
            'include perfetto module android.memory.heap_graph.class_tree;',
          ),
          ...metricsFromTableOrSubquery(
            `
              (
                select id, parent_id as parentId, name, self_size, self_count
                from _heap_graph_dominator_class_tree
                where graph_sample_ts = ${ts} and upid = ${upid}
              )
            `,
            [
              {
                name: 'Dominated Object Size',
                unit: 'B',
                columnName: 'self_size',
              },
              {
                name: 'Dominated Object Count',
                unit: '',
                columnName: 'self_count',
              },
            ],
            'include perfetto module android.memory.heap_graph.dominator_class_tree;',
          ),
        ],
      };
    }
    return m(
      '.flamegraph-profile',
      m(
        DetailsShell,
        {
          fillParent: true,
          title: m('.title', 'Java Heap Graph'),
          description: [],
          buttons: [m('.time', `Snapshot time: `, m(Timestamp, {ts}))],
        },
        m(QueryFlamegraph, assertExists(this.flamegraphAttrs)),
      ),
    );
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.HeapProfile',
  plugin: HeapProfilePlugin,
};
