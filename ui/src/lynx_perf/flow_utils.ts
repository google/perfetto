// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Engine} from '../trace_processor/engine';
import {Flow} from '../core/flow_types';
import {LONG, NUM, STR_NULL} from '../trace_processor/query_result';
import {Time} from '../base/time';
import {asSliceSqlId} from '../components/sql_utils/core_types';

export async function querySliceRelatedFlows(
  engine: Engine,
  sliceId: number,
): Promise<Flow[]> {
  const query = `
    -- Include slices.flow to initialise indexes on 'flow.slice_in' and 'flow.slice_out'.
    INCLUDE PERFETTO MODULE slices.flow;

    select
      f.slice_out as beginSliceId,
      t1.track_id as beginTrackId,
      t1.name as beginSliceName,
      t1.category as beginSliceCategory,
      t1.ts as beginSliceStartTs,
      (t1.ts+t1.dur) as beginSliceEndTs,
      thread_out.name as beginThreadName,
      f.slice_in as endSliceId,
      t2.track_id as endTrackId,
      t2.name as endSliceName,
      t2.category as endSliceCategory,
      t2.ts as endSliceStartTs,
      (t2.ts+t2.dur) as endSliceEndTs,
      thread_in.name as endThreadName,
      extract_arg(f.arg_set_id, 'cat') as category,
      extract_arg(f.arg_set_id, 'name') as name,
      f.id as id
    from directly_connected_flow(${sliceId}) f
    join slice t1 on f.slice_out = t1.slice_id
    join slice t2 on f.slice_in = t2.slice_id
    left join thread_track track_out on track_out.id = t1.track_id
    left join thread thread_out on thread_out.utid = track_out.utid
    left join thread_track track_in on track_in.id = t2.track_id
    left join thread thread_in on thread_in.utid = track_in.utid
    `;
  const result = await engine.query(query);
  const flows: Flow[] = [];

  const it = result.iter({
    beginSliceId: NUM,
    beginTrackId: NUM,
    beginSliceName: STR_NULL,
    beginSliceCategory: STR_NULL,
    beginSliceStartTs: LONG,
    beginSliceEndTs: LONG,
    beginThreadName: STR_NULL,
    endSliceId: NUM,
    endTrackId: NUM,
    endSliceName: STR_NULL,
    endSliceCategory: STR_NULL,
    endSliceStartTs: LONG,
    endSliceEndTs: LONG,
    endThreadName: STR_NULL,
    name: STR_NULL,
    category: STR_NULL,
    id: NUM,
  });

  const nullToStr = (s: null | string): string => {
    return s === null ? 'NULL' : s;
  };

  const nullToUndefined = (s: null | string): undefined | string => {
    return s === null ? undefined : s;
  };

  const nodes = [];

  for (; it.valid(); it.next()) {
    // Category and name present only in version 1 flow events
    // It is most likelly NULL for all other versions
    const category = nullToUndefined(it.category);
    const name = nullToUndefined(it.name);
    const id = it.id;

    const begin = {
      trackId: it.beginTrackId,
      sliceId: asSliceSqlId(it.beginSliceId),
      sliceName: nullToStr(it.beginSliceName),
      sliceCategory: nullToStr(it.beginSliceCategory),
      sliceStartTs: Time.fromRaw(it.beginSliceStartTs),
      sliceEndTs: Time.fromRaw(it.beginSliceEndTs),
      depth: 0,
      threadName: nullToStr(it.beginThreadName),
      processName: 'NULL',
      pipelineId: null,
    };

    const end = {
      trackId: it.endTrackId,
      sliceId: asSliceSqlId(it.endSliceId),
      sliceName: nullToStr(it.endSliceName),
      sliceCategory: nullToStr(it.endSliceCategory),
      sliceStartTs: Time.fromRaw(it.endSliceStartTs),
      sliceEndTs: Time.fromRaw(it.endSliceEndTs),
      depth: 0,
      threadName: nullToStr(it.endThreadName),
      processName: 'NULL',
      pipelineId: null,
    };

    nodes.push(begin);
    nodes.push(end);

    flows.push({
      id,
      begin,
      end,
      dur: it.endSliceStartTs - it.beginSliceEndTs,
      category,
      name,
      flowToDescendant: false,
    });
  }
  return flows;
}
