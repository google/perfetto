// Copyright (C) 2026 The Android Open Source Project
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

import {ThreadSliceDetailsPanel} from '../../components/details/thread_slice_details_tab';
import {SliceTrack} from '../../components/tracks/slice_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';

interface MarkerRow {
  trackId: number;
  utid: number;
  category: string;
  name: string;
}

// Renders Firefox Profiler markers (imported from gecko-format profiles) in
// the timeline. The gecko importer creates exactly one track per
// (utid, category, marker name) — this plugin lays those tracks out under
// their thread, sub-grouped by category when the thread has markers in
// multiple categories, mirroring the Firefox Profiler's marker chart.
export default class FirefoxProfilerMarkersPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.FirefoxProfilerMarkers';
  static readonly description =
    'Lays out Firefox profiler markers as per-thread, per-category timelines.';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const rows = await this.collectMarkerRows(ctx);
    if (rows.length === 0) return;

    // Count distinct categories per thread so we can skip the category-level
    // sub-group for threads that only have one category (the wrapper would
    // just be visual noise in that case).
    const categoriesPerThread = new Map<number, Set<string>>();
    for (const r of rows) {
      let s = categoriesPerThread.get(r.utid);
      if (s === undefined) {
        s = new Set();
        categoriesPerThread.set(r.utid, s);
      }
      s.add(r.category);
    }

    const threadGroups = ctx.plugins.getPlugin(ProcessThreadGroupsPlugin);
    const categoryGroups = new Map<string, TrackNode>();

    for (const r of rows) {
      const threadGroup = threadGroups.getGroupForThread(r.utid);
      if (threadGroup === undefined) continue;

      const useCategoryWrapper =
        (categoriesPerThread.get(r.utid)?.size ?? 1) > 1;
      const parent = useCategoryWrapper
        ? this.getOrCreateCategoryGroup(
            categoryGroups,
            threadGroup,
            r.utid,
            r.category,
          )
        : threadGroup;

      const uri = `/firefox_marker/${r.trackId}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: SliceTrack.create({
          trace: ctx,
          uri,
          rootTableName: 'slice',
          dataset: new SourceDataset({
            src: 'slice',
            schema: {
              ts: LONG,
              dur: LONG,
              name: STR,
              id: NUM,
              track_id: NUM,
              arg_set_id: NUM_NULL,
            },
            filter: {col: 'track_id', eq: r.trackId},
          }),
          detailsPanel: () => new ThreadSliceDetailsPanel(ctx),
        }),
        tags: {
          kinds: [SLICE_TRACK_KIND],
          trackIds: [r.trackId],
          utid: r.utid,
        },
      });
      parent.addChildInOrder(new TrackNode({uri, name: r.name, sortOrder: 30}));
    }
  }

  // Each (utid, category, name) is its own track; reading the dimensions
  // straight off the track table is all we need.
  private async collectMarkerRows(ctx: Trace): Promise<MarkerRow[]> {
    const result = await ctx.engine.query(`
      select
        t.id as trackId,
        extract_arg(t.dimension_arg_set_id, 'utid') as utid,
        extract_arg(t.dimension_arg_set_id, 'firefox_marker_category')
          as category,
        extract_arg(t.dimension_arg_set_id, 'firefox_marker_name') as name
      from track t
      where t.type = 'firefox_marker'
      order by utid, category, name
    `);

    const rows: MarkerRow[] = [];
    const it = result.iter({
      trackId: NUM,
      utid: NUM,
      category: STR,
      name: STR,
    });
    for (; it.valid(); it.next()) {
      rows.push({
        trackId: it.trackId,
        utid: it.utid,
        category: it.category,
        name: it.name,
      });
    }
    return rows;
  }

  private getOrCreateCategoryGroup(
    cache: Map<string, TrackNode>,
    threadGroup: TrackNode,
    utid: number,
    category: string,
  ): TrackNode {
    const key = `${utid}:${category}`;
    let sub = cache.get(key);
    if (sub === undefined) {
      sub = new TrackNode({name: category, sortOrder: 30});
      threadGroup.addChildInOrder(sub);
      cache.set(key, sub);
    }
    return sub;
  }
}
