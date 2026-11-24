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

import {describe, expect, test} from 'vitest';
import type {Filter} from '../../components/widgets/datagrid/model';
import {facetFilters, readValueSet, writeValueSet} from './value_set_filters';

const size: Filter = {field: 'size_bytes', op: '>', value: 10};

describe('value-set filters', () => {
  test('writeValueSet appends one `in` filter and keeps the others', () => {
    const out = writeValueSet([size], 'trace_population', ['A', 'B']);
    expect(out).toEqual([
      size,
      {field: 'trace_population', op: 'in', value: ['A', 'B']},
    ]);
    expect(readValueSet(out, 'trace_population')).toEqual(['A', 'B']);
  });

  test('writing an empty set removes the filter', () => {
    const withPop = writeValueSet([size], 'trace_population', ['A']);
    expect(writeValueSet(withPop, 'trace_population', [])).toEqual([size]);
  });

  test('readValueSet also reads a lone `=` which writeValueSet replaces', () => {
    const eq: Filter = {field: 'device_manufacturer', op: '=', value: 'Google'};
    expect(readValueSet([eq], 'device_manufacturer')).toEqual(['Google']);
    expect(writeValueSet([eq], 'device_manufacturer', ['Samsung'])).toEqual([
      {field: 'device_manufacturer', op: 'in', value: ['Samsung']},
    ]);
  });

  test('facetFilters drops only the picker\u2019s own columns', () => {
    const filters = writeValueSet(
      writeValueSet([size], 'device_manufacturer', ['Google']),
      'device_marketing_name',
      ['Pixel 8'],
    );
    expect(
      facetFilters(filters, 'device_manufacturer', 'device_marketing_name'),
    ).toEqual([size]);
  });
});
