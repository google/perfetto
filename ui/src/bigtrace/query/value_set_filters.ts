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

// The corpus filter pickers store what they pick as one `in` filter per
// trace-metadata column on the tab, alongside whatever else the grid holds.
// This is the mapping between those filters and a picker's value set.

import type {Filter} from '../../components/widgets/datagrid/model';

// Trace-metadata columns the corpus filter bar knows how to offer a picker
// for. A chip is rendered only when the backend's schema has its column(s).
export const POPULATION_COLUMN = 'trace_population';
export const FORM_FACTOR_COLUMN = 'form_factor';
export const MANUFACTURER_COLUMN = 'device_manufacturer';
export const MODEL_COLUMN = 'device_marketing_name';

function isValueSetFilter(f: Filter, field: string): boolean {
  return f.field === field && (f.op === 'in' || f.op === '=');
}

// The values picked for `field`: its `in` list, or a lone `=` value. Other
// ops on the column (glob, null checks) are not the picker's business.
export function readValueSet(
  filters: ReadonlyArray<Filter>,
  field: string,
): string[] {
  const f = filters.find((f) => isValueSetFilter(f, field));
  if (f === undefined) return [];
  if (f.op === 'in') return f.value.map(String);
  if (f.op === '=') return [String(f.value)];
  return [];
}

// Replace the picked values for `field`; an empty set removes the filter.
export function writeValueSet(
  filters: ReadonlyArray<Filter>,
  field: string,
  values: ReadonlyArray<string>,
): Filter[] {
  const rest = filters.filter((f) => !isValueSetFilter(f, field));
  if (values.length === 0) return rest;
  return [...rest, {field, op: 'in', value: [...values]}];
}

// What a picker's option list is narrowed by: every filter except the ones
// on the columns it edits, so it offers only values that exist under the
// other chips without hiding its own alternatives.
export function facetFilters(
  filters: ReadonlyArray<Filter>,
  ...ownFields: string[]
): Filter[] {
  return filters.filter((f) => !ownFields.includes(f.field));
}
