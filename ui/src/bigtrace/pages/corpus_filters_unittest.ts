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
import m from 'mithril';
import type {ColumnValuesLoader} from '../query/column_values_loader';
import {
  FORM_FACTOR_COLUMN,
  MANUFACTURER_COLUMN,
  MODEL_COLUMN,
} from '../query/value_set_filters';
import {describePopulation} from './corpus_filters';
import {
  DeviceFilterDialog,
  describeDevice,
  describeFormFactors,
} from './device_filter_modal';

describe('chip labels', () => {
  test('describePopulation', () => {
    expect(describePopulation([])).toBe('Population: All');
    expect(describePopulation(['A'])).toBe('Population: A');
    expect(describePopulation(['A', 'B'])).toBe('Population: 2 selected');
  });

  test('describeDevice', () => {
    expect(describeDevice([], [], [])).toBe('Device: All');
    expect(describeDevice([], ['Google'], [])).toBe('Device: Google');
    expect(describeDevice([], [], ['Pixel 8'])).toBe('Device: Pixel 8');
    expect(describeDevice(['PHONE'], ['Google'], ['Pixel 8', 'Pixel 9'])).toBe(
      'Device: PHONE · Google · 2 models',
    );
    expect(describeDevice([], ['Google', 'Samsung'], [])).toBe(
      'Device: 2 manufacturers',
    );
  });

  test('describeFormFactors keeps the dropdown button short', () => {
    expect(describeFormFactors([])).toBe('Any');
    expect(describeFormFactors(['PHONE'])).toBe('PHONE');
    expect(describeFormFactors(['PHONE', 'TABLET'])).toBe('PHONE, TABLET');
    expect(describeFormFactors(['PHONE', 'TABLET', 'WATCH'])).toBe(
      '3 selected',
    );
  });
});

describe('device picker lifetime', () => {
  test('closing the dialog aborts every value load, form factors included', async () => {
    const root = document.createElement('div');
    document.body.appendChild(root);
    const loads: {column: string; signal?: AbortSignal}[] = [];
    // Never settles: the loads are still in flight when the dialog closes.
    const load: ColumnValuesLoader = (column, _facet, _search, signal) => {
      loads.push({column, signal});
      return new Promise(() => {});
    };
    const draft = {
      formFactors: new Set<string>(),
      manufacturers: new Set(['Google']),
      models: new Set<string>(),
    };
    m.mount(root, {
      view: () =>
        m(DeviceFilterDialog, {
          draft,
          load,
          facet: [],
          formFactor: true,
          onApply: () => {},
          onCancel: () => {},
        }),
    });
    // The form-factor compute is queued by AsyncMemo; let it start.
    await Promise.resolve();
    await Promise.resolve();
    expect(loads.map((l) => l.column).sort()).toEqual(
      [FORM_FACTOR_COLUMN, MANUFACTURER_COLUMN, MODEL_COLUMN].sort(),
    );
    for (const l of loads) {
      expect(l.signal).toBeDefined();
      expect(l.signal?.aborted).toBe(false);
    }

    m.mount(root, null);
    root.remove();
    for (const l of loads) expect(l.signal?.aborted).toBe(true);
  });
});
