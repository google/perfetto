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

import type {Trace} from '../../public/trace';
import {metricsFromTableOrSubquery, QueryFlamegraph} from '../query_flamegraph';
import {
  FLAMEGRAPH_STATE_SCHEMA,
  type FlamegraphState,
} from '../../widgets/flamegraph';

export interface AddQueryFlamegraphArgs {
  readonly trace: Trace;
  readonly title: string;
  // SQL subquery exposing the user's data. Treated as a TableOrSubquery.
  readonly sourceQuery: string;
  readonly idColumn: string;
  readonly parentIdColumn: string;
  readonly nameColumn: string;
  readonly valueColumn: string;
  // Metric metadata - shows up in pprof and in the flamegraph header.
  readonly sampleType: string;
  readonly unit: string;
}

// Opens an ephemeral Flamegraph tab driven by a user-supplied SQL query.
// The tab reuses the same `QueryFlamegraph` component used by the heap
// profile / java heap / slice flamegraph paths, so it inherits the
// shared "Download pprof" toolbar button.
export function addQueryFlamegraphTab(args: AddQueryFlamegraphArgs): void {
  const {
    trace,
    title,
    sourceQuery,
    idColumn,
    parentIdColumn,
    nameColumn,
    valueColumn,
    sampleType,
    unit,
  } = args;

  // The flamegraph contract expects columns named exactly id, parentId,
  // name and selfValue. Project the user's chosen columns onto that
  // shape via a subquery.
  const projectedSubquery = `(
    select
      ${idColumn} as id,
      ${parentIdColumn} as parentId,
      ${nameColumn} as name,
      ${valueColumn} as selfValue
    from (${sourceQuery})
  )`;

  const metrics = metricsFromTableOrSubquery({
    tableMetrics: [{name: sampleType, unit, columnName: 'selfValue'}],
    tableOrSubquery: projectedSubquery,
  });

  const flamegraph = new QueryFlamegraph(trace);
  let state: FlamegraphState = FLAMEGRAPH_STATE_SCHEMA.parse({
    selectedMetricName: sampleType,
    filters: [],
    view: {kind: 'TOP_DOWN'},
  });
  const uri = `query_flamegraph#${title}-${Date.now()}`;
  trace.tabs.registerTab({
    uri,
    isEphemeral: true,
    content: {
      getTitle: () => title,
      render: () =>
        flamegraph.render({
          metrics,
          state,
          onStateChange: (next: FlamegraphState) => {
            state = next;
          },
        }),
    },
  });
  trace.tabs.showTab(uri);
}
