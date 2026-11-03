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
import {STITCHED_CPU_SAMPLES_MODULE} from './cpu_samples_sql';
import {flowsModules, flowsPipeline} from './flows';
import {
  StitchedPipeline,
  stitchedMetric,
  type StitchedMetricProvider,
} from './metrics';

// CPU samples taken in the slices stitched by flows, as callstacks below the
// slices: downstream, the code that ran on behalf of the selection on every
// thread; upstream, the code of the selection with what led to it below.
export const CPU_SAMPLES_PROVIDER: StitchedMetricProvider = {
  async isAvailable(engine) {
    const res = await engine.query(`
      select exists(
        select 1 from stack_sample where callsite_id is not null
      ) as available
    `);
    return res.firstRow({available: NUM}).available === 1;
  },
  modules(selection) {
    return [...flowsModules(selection), STITCHED_CPU_SAMPLES_MODULE];
  },
  metrics(trace, selection, prefix) {
    const {start, end} = selection;
    const flows = flowsPipeline(prefix, selection);
    const nodes = flows.nodes;
    const p = new StitchedPipeline(`${prefix}_samples`, [
      'callstacks.stack_profile',
      'intervals.intersect',
    ]);
    // Upstream, only the selected slices (the weighted nodes) own samples.
    const targetIds =
      selection.direction === 'DOWN'
        ? `(select id from ${nodes})`
        : `(select id from ${nodes} where self_count = 1)`;
    const threads = p.table(
      'threads',
      `select distinct utid
      from _stitched_track
      where track_id in (${selection.trackIds.join(',')})
        and utid is not null`,
    );
    const targets = p.table(
      'targets',
      `select * from _stitched_samples_targets!(
        ${nodes}, ${targetIds}, ${end})`,
      'id',
    );
    const samples = p.table(
      'samples',
      `select * from _stitched_samples_in!(
        ${targets}, ${threads}, ${start}, ${end})`,
    );
    const attributed = p.table(
      'attributed',
      `select * from _stitched_samples_attribute!(
        ${samples}, ${targets}, ${threads})`,
    );
    const frames = p.table(
      'frames',
      `select * from _stitched_samples_frames!(${attributed})`,
    );
    const result = p.table(
      'result',
      `select * from _stitched_samples_nodes!(
        ${nodes}, ${attributed}, ${frames})`,
      'id',
    );
    return {
      prepare: [flows.pipeline],
      metrics: [
        stitchedMetric(trace, selection, {
          id: 'cpu_samples',
          name: 'CPU Samples',
          unit: '',
          valueColumn: 'self_count',
          pipeline: p,
          nodes: result,
        }),
      ],
      tables: [...flows.pipeline.tables, ...p.tables],
    };
  },
};
