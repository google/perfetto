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

import type {QueryFlamegraphMetric} from '../../components/query_flamegraph';

// Maps pprof units onto the flamegraph's unit vocabulary ('ns' and 'B' are
// human-scaled by displaySize).
export function displayUnit(unit: string): string {
  switch (unit.toLowerCase()) {
    case 'nanoseconds':
      return 'ns';
    case 'bytes':
      return 'B';
    default:
      return unit;
  }
}

// Flamegraph metric summing the given aggregate profiles. Callsites are
// interned globally, so a single query merges same-stack samples across
// profiles.
export function aggregateProfileMetric(
  name: string,
  unit: string,
  aggIds: ReadonlyArray<number>,
): QueryFlamegraphMetric {
  return {
    name,
    unit: displayUnit(unit),
    nameColumnLabel: 'Symbol',
    dependencySql: 'include perfetto module callstacks.stack_profile',
    statement: `
      WITH profile_samples AS MATERIALIZED (
        SELECT callsite_id, sum(sample.value) AS sample_value
        FROM __intrinsic_aggregate_sample sample
        WHERE sample.aggregate_profile_id IN (${aggIds.join(',')})
        GROUP BY callsite_id
      )
      SELECT
        c.id,
        c.parent_id as parentId,
        c.name,
        c.mapping_name,
        c.source_file || ':' || c.line_number as source_location,
        cast_string!(c.inlined) AS inlined,
        CASE WHEN c.is_leaf_function_in_callsite_frame
          THEN coalesce(m.sample_value, 0)
          ELSE 0
        END AS value
      FROM _callstacks_for_stack_profile_samples!(profile_samples) AS c
      LEFT JOIN profile_samples AS m USING (callsite_id)
    `,
    unaggregatableProperties: [
      {name: 'mapping_name', displayName: 'Mapping'},
      {name: 'inlined', displayName: 'Inlined', isVisible: () => false},
    ],
    aggregatableProperties: [
      {
        name: 'source_location',
        displayName: 'Source Location',
        mergeAggregation: 'ONE_OR_SUMMARY',
      },
    ],
    optionalMarker: {
      name: 'Inlined Function',
      isVisible: (properties: ReadonlyMap<string, string>) =>
        properties.get('inlined') === '1',
    },
  };
}
