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

import {NUM, NUM_NULL} from '../trace_processor/query_result';
import {Engine} from '../trace_processor/engine';
import {BaseSlice} from './types';
import {Flow} from '../core/flow_types';

export async function findPrecedingIdenticalFlowIdSlice(
  engine: Engine,
  sliceId: number,
): Promise<BaseSlice | undefined> {
  const query = `
    -- Include slices.flow to initialise indexes on 'flow.slice_in' and 'flow.slice_out'.
    INCLUDE PERFETTO MODULE slices.flow;

    select
      t1.ts as ts,
      t1.id as id,
      t1.dur as dur,
      t1.arg_set_id as argSetId
    from preceding_flow(${sliceId}) f
    join slice t1 on f.slice_out = t1.slice_id
    `;
  const result = await engine.query(query);
  if (result.numRows() > 0) {
    return result.firstRow({
      ts: NUM,
      id: NUM,
      dur: NUM,
      argSetId: NUM_NULL,
    });
  }
  return undefined;
}

export async function findAfterwardIdenticalFlowIdSlice(
  engine: Engine,
  sliceId: number,
): Promise<BaseSlice | undefined> {
  const query = `
    -- Include slices.flow to initialise indexes on 'flow.slice_in' and 'flow.slice_out'.
    INCLUDE PERFETTO MODULE slices.flow;

    select
    t1.ts as ts,
      t1.id as id,
      t1.dur as dur,
      t1.arg_set_id as argSetId
    from directly_connected_flow(${sliceId}) f
    join slice t1 on f.slice_in = t1.slice_id
    `;
  const result = await engine.query(query);
  if (result.numRows() > 0) {
    return result.firstRow({
      ts: NUM,
      id: NUM,
      dur: NUM,
      argSetId: NUM,
    });
  }
  return undefined;
}

export async function traceEventWithSpecificArgValue(
  engine: Engine,
  traceNames: string[],
  argValue: string,
  endTs: number,
) {
  const traceNameList = traceNames.map((timing) => `'${timing}'`).join(',');
  const queryRes = await engine.query(
    `select 
    slice.name as name, 
    args.display_value as value
    from slice 
    inner join args on args.arg_set_id = slice.arg_set_id
    where slice.name in (${traceNameList}) and slice.ts < ${endTs} and value='${argValue}' limit 1`,
  );
  return queryRes.numRows() > 0;
}

/**
 * Thera are many pipelind_ids for 'LynxView.onDraw', we should filter the pipeline Flow which is
 * not the same with 'Timing::Mark.paintEnd'
 * @param engine
 * @param flow
 */
export async function filterUncessaryPipleineFlow(
  engine: Engine,
  flow: Flow[],
  pipelineId: string | null,
) {
  if (!pipelineId) {
    return;
  }
  const filterPipelineIds: string[] = [];

  flow.forEach((item) => {
    item.begin.pipelineIds?.forEach((id) => {
      if (id !== pipelineId && !filterPipelineIds.includes(id)) {
        filterPipelineIds.push(id);
      }
    });
    item.end.pipelineIds?.forEach((id) => {
      if (id !== pipelineId && !filterPipelineIds.includes(id)) {
        filterPipelineIds.push(id);
      }
    });
  });
  if (filterPipelineIds.length <= 0) {
    return;
  }
  const flowNeedsToFilter: Flow[] = [];
  flow.forEach((item) => {
    if (filterPipelineIds.includes(item.begin.pipelineId || '')) {
      flowNeedsToFilter.push(item);
    }
  });

  const filterTraceIdList: number[] = [];
  // find the trace which has the above filterPipelineIds
  flow.forEach((item) => {
    if (filterPipelineIds.includes(item.begin.pipelineId || '')) {
      filterTraceIdList.push(item.begin.sliceId);
    }
  });
  if (filterTraceIdList.length <= 0) {
    return;
  }
  // find all the proceding flow for the above filterTraceIdList
  const filterProcedingFlow: ProcedingFlow[] = [];
  for (const id of filterTraceIdList) {
    filterProcedingFlow.push(...(await queryProcedingFlows(engine, id)));
  }

  // remove the item in flow which satisfies the following condition:
  // [item.begin.sliceid, item.end.sliceId] is in filterProcedingFlow
  for (let i = flow.length - 1; i >= 0; i--) {
    const item = flow[i];
    if (
      filterProcedingFlow.some(
        (f) =>
          f.beginSliceId === item.begin.sliceId &&
          f.endSliceId === item.end.sliceId,
      )
    ) {
      flow.splice(i, 1);
    }
  }

  // remove the flow which has the pipelineId in filterPipelineIds
  for (let i = flow.length - 1; i >= 0; i--) {
    if (flowNeedsToFilter.includes(flow[i])) {
      flow.splice(i, 1);
    }
  }
}

interface ProcedingFlow {
  beginSliceId: number;
  endSliceId: number;
}

export async function queryProcedingFlows(
  engine: Engine,
  sliceId: number,
): Promise<ProcedingFlow[]> {
  const query = `
    -- Include slices.flow to initialise indexes on 'flow.slice_in' and 'flow.slice_out'.
    INCLUDE PERFETTO MODULE slices.flow;
    select
      f.slice_out as beginSliceId,
      f.slice_in as endSliceId
      from preceding_flow(${sliceId}) f
    `;
  const result = await engine.query(query);
  const it = result.iter({
    beginSliceId: NUM,
    endSliceId: NUM,
  });
  const flows: ProcedingFlow[] = [];
  for (; it.valid(); it.next()) {
    flows.push({
      beginSliceId: it.beginSliceId,
      endSliceId: it.endSliceId,
    });
  }
  return flows;
}
