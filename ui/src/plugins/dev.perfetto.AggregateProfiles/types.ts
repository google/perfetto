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

import type {QueryFlamegraphMetric} from '../../components/query_flamegraph';
import {FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';
import {z} from 'zod';

export const AGGREGATE_PROFILES_PAGE_STATE_SCHEMA = z.object({
  flamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
  selectedProfileId: z.string().optional(),
});

export type AggregateProfilesPageState = z.infer<
  typeof AGGREGATE_PROFILES_PAGE_STATE_SCHEMA
>;

export interface AggregateProfile {
  readonly id: string;
  readonly displayName: string;
  readonly metrics: ReadonlyArray<QueryFlamegraphMetric>;
}

// Persisted page state. `columns`/`filters` mirror the DataGrid's controlled
// props ({id,field,sort} / {field,op,value}); kept loose here and cast at the
// use site. Filters are the single source of truth for the working set.
export const MERGE_PAGE_STATE_SCHEMA = z.object({
  flamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
  merge: z.boolean().default(true),
  columns: z.array(z.any()).optional(),
  filters: z.array(z.any()).default([]),
});
export type MergePageState = z.infer<typeof MERGE_PAGE_STATE_SCHEMA>;

// The derived total for one (profile, sample-type). `aggId` is the
// __intrinsic_aggregate_profile row carrying this profile's samples for this
// sample-type; it's what the flamegraph merge query filters on.
export interface MergeProfileMetric {
  readonly aggId: number;
  readonly total: number; // SUM(aggregate_sample.value)
  readonly count: number; // number of sample rows
}

// One source pprof (keyed by its file scope) with its derived sample-type
// totals.
export interface MergeProfile {
  readonly scope: string;
  readonly sampleTypes: ReadonlyMap<string, MergeProfileMetric>;
}

// A pprof sample-type present across the loaded profiles (mergeable target).
export interface SampleType {
  readonly key: string; // "cpu (nanoseconds)"
  readonly type: string; // "cpu"
  readonly unit: string; // "nanoseconds"
}

// A DataGrid column, plus the metadata the crossfilter chart stack needs to
// render the right chart and (for sample-types) find the aggregate id to merge.
export interface MergeColumn {
  readonly field: string; // grid field id, no dots (e.g. "cpu")
  readonly title: string; // display title (e.g. "cpu (nanoseconds)")
  readonly kind: 'id' | 'numeric';
  readonly unit?: string; // numeric unit for formatting
  readonly sampleKey?: string; // for sample-type columns: the SampleType.key
}
