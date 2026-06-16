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

import m from 'mithril';
import type {Trace} from '../../../public/trace';
import type {time} from '../../../base/time';
import type {QueryFlamegraphMetric} from '../../../components/query_flamegraph';
import {FlamegraphPanel} from '../../../components/flamegraph_panel';
import {Flamegraph, type FlamegraphState} from '../../../widgets/flamegraph';
import {Stack} from '../../../widgets/stack';
import {STR} from '../../../trace_processor/query_result';

interface OomCallstackViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly ts: time;
  readonly state: FlamegraphState | undefined;
  readonly onStateChange: (state: FlamegraphState) => void;
}

function buildOomCallstackMetrics(
  ts: time,
): ReadonlyArray<QueryFlamegraphMetric> {
  return [
    {
      name: 'OOM Callstack',
      unit: '',
      statement: `
        WITH RECURSIVE callstack AS (
          SELECT c.id, c.parent_id as parentId, c.depth, c.frame_id
          FROM stack_profile_callsite c
          WHERE c.id = (
            SELECT callsite_id
            FROM heap_graph_thread_callsite hc
            JOIN heap_graph g ON hc.heap_graph_id = g.id
            WHERE g.ts = ${ts}
            LIMIT 1
          )

          UNION ALL

          SELECT c.id, c.parent_id as parentId, c.depth, c.frame_id
          FROM stack_profile_callsite c
          JOIN callstack ON c.id = callstack.parentId
        )
        SELECT
          cs.id,
          cs.parentId,
          ifnull(f.name, '[Unknown]') as name,
          iif(cs.depth = (select max(depth) from callstack), 1, 0) as value,
          s.source_file || ':' || cast(s.line_number as text) as source_location
        FROM callstack cs
        JOIN stack_profile_frame f ON cs.frame_id = f.id
        LEFT JOIN stack_profile_symbol s ON f.symbol_set_id = s.symbol_set_id
      `,
      unaggregatableProperties: [],
      aggregatableProperties: [
        {
          name: 'source_location',
          displayName: 'Source Location',
          mergeAggregation: 'ONE_OR_SUMMARY',
        },
      ],
    },
  ];
}

const OomCallstackView: m.ClosureComponent<OomCallstackViewAttrs> = () => {
  let cachedMetrics: ReadonlyArray<QueryFlamegraphMetric> | undefined;
  let cachedKey: string | undefined;
  let oomErrorMsg: string | undefined;

  async function loadErrorMsg(trace: Trace, upid: number, ts: time) {
    try {
      const errorMsgRes = await trace.engine.query(`
        INCLUDE PERFETTO MODULE android.memory.heap_graph.oome;
        SELECT oome.error_msg 
        FROM heap_graph g
        JOIN android_heap_graph_java_oome_details oome ON oome.heap_graph_id = g.id
        WHERE g.upid = ${upid} AND g.ts = ${ts}
        LIMIT 1
      `);
      oomErrorMsg =
        errorMsgRes.numRows() > 0
          ? errorMsgRes.firstRow({error_msg: STR}).error_msg
          : undefined;
      m.redraw();
    } catch (e) {
      console.error('Failed to load OOM error message:', e);
    }
  }

  return {
    view({attrs}) {
      const key = `${attrs.upid}:${attrs.ts}`;
      if (cachedMetrics === undefined || key !== cachedKey) {
        cachedMetrics = buildOomCallstackMetrics(attrs.ts);
        cachedKey = key;
        oomErrorMsg = undefined;
        loadErrorMsg(attrs.trace, attrs.upid, attrs.ts);
      }
      const metrics = cachedMetrics;

      let state = attrs.state;
      if (state === undefined) {
        state = Flamegraph.createDefaultState(metrics);
        attrs.onStateChange(state);
      }

      return m(
        'div',
        {class: 'pf-hde-view-content pf-hde-flamegraph-view'},
        m(
          Stack,
          {orientation: 'vertical'},
          oomErrorMsg &&
            m(
              'div',
              {style: {padding: '8px', fontSize: '14px', color: '#ff4081'}},
              oomErrorMsg,
            ),
          m(FlamegraphPanel, {
            trace: attrs.trace,
            metrics,
            state,
            onStateChange: attrs.onStateChange,
          }),
        ),
      );
    },
  };
};

export default OomCallstackView;
