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

import {beforeEach, describe, expect, test} from 'vitest';
import {z} from 'zod';
import {
  disabledSettingsFromSnapshot,
  effectiveTabSettings,
  type BigTraceEditorTab,
} from './query_tabs_state';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';

function reg(id: string, defaultValue: unknown, type: 'string' | 'number') {
  return bigTraceSettingsStorage.register({
    id,
    name: id,
    description: '',
    type,
    schema: (type === 'number' ? z.number() : z.string()) as never,
    defaultValue,
    category: 'TRACE_ADDRESS',
  });
}

function fakeTab(over: Partial<BigTraceEditorTab>): BigTraceEditorTab {
  return {
    querySettings: [],
    traceFilters: [],
    traceMetadataColumns: [],
    traceOrderBy: '',
    disabledSettings: [],
    ...over,
  } as unknown as BigTraceEditorTab;
}

describe('effectiveTabSettings (per-tab settings)', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  test('merges global defaults with per-tab value overrides', () => {
    reg('trace_directory', '/global', 'string');
    const tab = fakeTab({
      querySettings: [
        {
          settingId: 'trace_directory',
          values: ['/per-tab'],
          category: 'TRACE_ADDRESS',
        },
      ],
    });
    expect(
      effectiveTabSettings(tab).find((s) => s.settingId === 'trace_directory')
        ?.values,
    ).toEqual(['/per-tab']);
  });

  test('excludes per-tab-disabled settings WITHOUT changing the global state', () => {
    const dir = reg('trace_directory', '/global', 'string');
    const limit = reg('trace_limit', 100, 'number');
    const tab = fakeTab({disabledSettings: ['trace_limit']});
    const ids = effectiveTabSettings(tab).map((s) => s.settingId);
    expect(ids).toContain('trace_directory');
    expect(ids).not.toContain('trace_limit'); // dropped for this tab
    // Independence: the GLOBAL settings stay enabled.
    expect(limit.isDisabled()).toBe(false);
    expect(dir.isDisabled()).toBe(false);
  });

  test('a globally-disabled setting can be re-enabled per-tab', () => {
    const limit = reg('trace_limit', 100, 'number');
    limit.setDisabled(true); // globally OFF
    const tab = fakeTab({disabledSettings: []}); // tab leaves it on
    expect(effectiveTabSettings(tab).map((s) => s.settingId)).toContain(
      'trace_limit',
    );
  });
});

describe('disabledSettingsFromSnapshot (history reopen)', () => {
  test('disabled set is every categoried setting the snapshot omits', () => {
    expect(
      disabledSettingsFromSnapshot(
        ['trace_directory'], // active at submit time
        ['trace_directory', 'trace_limit', 'cpu_filter'], // all categoried
      ).sort(),
    ).toEqual(['cpu_filter', 'trace_limit']);
  });

  test('nothing disabled when the snapshot covers every categoried setting', () => {
    expect(
      disabledSettingsFromSnapshot(
        ['trace_directory', 'trace_limit'],
        ['trace_directory', 'trace_limit'],
      ),
    ).toEqual([]);
  });

  test('round-trips effectiveTabSettings: a tab-disabled setting reads back disabled', () => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
    reg('trace_directory', '/global', 'string');
    reg('trace_limit', 100, 'number');
    // A tab that turned trace_limit OFF — this is what a run submits.
    const tab = fakeTab({disabledSettings: ['trace_limit']});
    const snapshot = effectiveTabSettings(tab); // == submit-time settings
    const allCategoried = bigTraceSettingsStorage
      .buildSettingFilters({includeDisabled: true})
      .map((s) => s.settingId);
    // Reopening from history must recover the same disabled set.
    expect(
      disabledSettingsFromSnapshot(
        snapshot.map((s) => s.settingId),
        allCategoried,
      ),
    ).toEqual(['trace_limit']);
  });
});

describe('boolean settings have no enable/disable concept', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  test('a boolean reports not-disabled even after setDisabled(true)', () => {
    const flag = bigTraceSettingsStorage.register({
      id: 'my_flag',
      name: 'my_flag',
      description: '',
      type: 'boolean',
      schema: z.boolean() as never,
      defaultValue: false,
      category: 'BIGTRACE_QUERY_OPTIONS',
    });
    flag.setDisabled(true);
    // Booleans ignore enable/disable: isDisabled() stays false so the control
    // stays editable, and the setting stays in the effective set.
    expect(flag.isDisabled()).toBe(false);
    expect(effectiveTabSettings(fakeTab({})).map((s) => s.settingId)).toContain(
      'my_flag',
    );
  });
});
