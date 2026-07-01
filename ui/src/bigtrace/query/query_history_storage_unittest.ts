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
import {
  snapshotSettingsToFilters,
  type SnapshotSettingEntry,
} from './query_history_storage';

// The snapshot is echoed with camelCase `settingId`, like every wire field.
// Reading the wrong casing drops every entry, losing the query's settings.
describe('snapshotSettingsToFilters', () => {
  test('parses camelCase settingId into SettingFilter', () => {
    expect(
      snapshotSettingsToFilters([
        {settingId: 'trace_limit', values: ['100'], category: 'TRACE_ADDRESS'},
      ]),
    ).toEqual([
      {settingId: 'trace_limit', values: ['100'], category: 'TRACE_ADDRESS'},
    ]);
  });

  test('coerces non-string values to strings; null becomes ""', () => {
    expect(
      snapshotSettingsToFilters([
        {
          settingId: 'n',
          values: [42, true, null],
          category: 'BIGTRACE_QUERY_OPTIONS',
        },
      ]),
    ).toEqual([
      {
        settingId: 'n',
        values: ['42', 'true', ''],
        category: 'BIGTRACE_QUERY_OPTIONS',
      },
    ]);
  });

  test('drops malformed entries; undefined input becomes []', () => {
    expect(snapshotSettingsToFilters(undefined)).toEqual([]);
    const entries = [
      {values: ['x'], category: 'TRACE_ADDRESS'}, // missing settingId
      {settingId: 'ok', values: ['y'], category: 'TRACE_ADDRESS'},
    ] as unknown as SnapshotSettingEntry[];
    expect(snapshotSettingsToFilters(entries)).toEqual([
      {settingId: 'ok', values: ['y'], category: 'TRACE_ADDRESS'},
    ]);
  });
});
