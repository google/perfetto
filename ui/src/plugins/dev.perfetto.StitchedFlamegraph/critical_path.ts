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

import {NUM} from '../../trace_processor/query_result';
import {STITCHED_CRITICAL_PATH_MODULE} from './critical_path_sql';
import {
  StitchedPipeline,
  stitchedMetric,
  type StitchedMetricProvider,
} from './metrics';

// Slices stitched through the scheduler's critical path, weighted by time on
// the critical path.
export const CRITICAL_PATH_PROVIDER: StitchedMetricProvider = {
  async isAvailable(engine) {
    // The critical path is computed from scheduling and wakeup events.
    const res = await engine.query(`
      select exists(select 1 from sched) and exists(
        select 1 from thread_state where waker_utid is not null
      ) as available
    `);
    return res.firstRow({available: NUM}).available === 1;
  },
  modules() {
    return [STITCHED_CRITICAL_PATH_MODULE];
  },
  metrics(trace, selection, prefix) {
    const {start, end} = selection;
    const p = new StitchedPipeline(`${prefix}_cp`);
    const tracks = p.table(
      'tracks',
      `select track_id
      from _stitched_track
      where track_id in (${selection.trackIds.join(',')})`,
    );
    const threads = p.table(
      'threads',
      `select * from _stitched_cp_threads!(${tracks})`,
      'utid',
    );
    let nodes: string;
    if (selection.direction === 'DOWN') {
      const seeds = p.table(
        'seeds',
        `select * from _stitched_cp_seeds!(${tracks}, ${start}, ${end})`,
        'id',
      );
      const roots = p.table(
        'roots',
        `select * from _stitched_cp_down_roots!(${threads}, ${start}, ${end})`,
        'root_node_id',
      );
      const allFrames = p.table(
        'all_frames',
        `select * from _critical_path_with_depth_by_roots!(
          ${roots}, _wakeup_graph)`,
      );
      const frames = p.table(
        'frames',
        `select * from _stitched_cp_window_frames!(
          ${allFrames}, ${roots}, ${start}, ${end})`,
      );
      const flat = p.table(
        'flat',
        `select * from _stitched_cp_flatten!(${frames})`,
      );
      const pieces = p.table(
        'pieces',
        `select * from _stitched_cp_pieces!(
          ${flat}, ${threads}, FALSE, ${start}, ${end})`,
      );
      const blocked = p.table(
        'blocked',
        `select * from _stitched_cp_blocked_slice!(${roots})`,
        'root_node_id',
      );
      const attached = p.table(
        'attached',
        `select * from _stitched_cp_attach!(${pieces}, ${blocked})`,
        'piece_id',
      );
      const hits = p.table(
        'hits',
        `select * from _stitched_cp_hits!(${attached})`,
      );
      const canonical = p.table(
        'canonical',
        `select * from _stitched_cp_canonical!(${hits})`,
        'slice_id',
      );
      const placed = p.table(
        'placed',
        `select * from _stitched_cp_place!(${hits}, ${attached}, ${canonical})`,
      );
      const groups = p.table(
        'groups',
        `select * from _stitched_cp_blocker_groups!(${placed}, ${canonical})`,
        'parent_node_id, slice_id',
      );
      const covered = p.table(
        'covered',
        `select * from _stitched_cp_piece_covered!(${placed})`,
        'piece_id',
      );
      nodes = p.table(
        'nodes',
        `select * from _stitched_cp_down_nodes!(
          ${seeds}, ${attached}, ${groups}, ${covered})`,
        'id',
      );
    } else {
      const roots = p.table(
        'roots',
        `select * from _stitched_cp_up_roots!(${threads}, ${start}, ${end})`,
      );
      const allFrames = p.table(
        'all_frames',
        `select * from _critical_path_with_depth_by_roots!(
          ${roots}, _wakeup_graph)`,
      );
      const hitRoots = p.table(
        'hit_roots',
        `select * from _stitched_cp_hit_roots!(
          ${allFrames}, ${threads}, ${start}, ${end})`,
        'root_node_id',
      );
      const frames = p.table(
        'frames',
        `select * from _stitched_cp_window_frames!(
          ${allFrames}, ${hitRoots}, ${start}, ${end})`,
      );
      const flat = p.table(
        'flat',
        `select * from _stitched_cp_flatten!(${frames})`,
      );
      const pieces = p.table(
        'pieces',
        `select * from _stitched_cp_pieces!(
          ${flat}, ${threads}, TRUE, ${start}, ${end})`,
      );
      // Upstream, the blocked slices are not weighted: merge the ones blocked
      // at the same place early.
      const blocked = p.table(
        'blocked',
        `select * from _stitched_cp_canonical_blocked!(
          (select * from _stitched_cp_blocked_slice!(${hitRoots})))`,
        'root_node_id',
      );
      const attached = p.table(
        'attached',
        `select * from _stitched_cp_attach!(${pieces}, ${blocked})`,
        'piece_id',
      );
      const hits = p.table(
        'hits',
        `select * from _stitched_cp_hits!(${attached})`,
      );
      const canonical = p.table(
        'canonical',
        `select * from _stitched_cp_canonical!(${hits})`,
        'slice_id',
      );
      const placed = p.table(
        'placed',
        `select * from _stitched_cp_place!(${hits}, ${attached}, ${canonical})`,
      );
      const groups = p.table(
        'groups',
        `select * from _stitched_cp_blocker_groups!(${placed}, ${canonical})`,
        'parent_node_id, slice_id',
      );
      const covered = p.table(
        'covered',
        `select * from _stitched_cp_piece_covered!(${placed})`,
        'piece_id',
      );
      nodes = p.table(
        'nodes',
        `select * from _stitched_cp_up_nodes!(
          ${blocked}, ${attached}, ${groups}, ${covered})`,
        'id',
      );
    }
    return {
      prepare: [],
      metrics: [
        stitchedMetric(trace, selection, {
          id: 'critical_path',
          name: 'Critical Path',
          unit: 'ns',
          valueColumn: 'self_dur',
          pipeline: p,
          nodes,
        }),
      ],
      tables: p.tables,
    };
  },
};
