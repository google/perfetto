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
  isLocalPreset,
  launcherPresets,
  matchingPresetId,
  preselectedPresetId,
} from './query_launcher';
import {
  applyPresetToTab,
  MODE_DEFAULTS,
  effectiveTraceLimit,
  type BigTraceEditorTab,
} from './query_tabs_state';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import type {TracePreset} from '../query/bigtrace_query_client';
import type {LocalPreset} from '../query/local_preset_store';

function preset(over: Partial<TracePreset> = {}): TracePreset {
  return {
    id: 'backend-1',
    category: 'Android',
    name: 'Jank',
    description: '',
    perfettoSql: 'select 1',
    ...over,
  };
}

function local(over: Partial<LocalPreset> = {}): LocalPreset {
  return {...preset({id: 'local:1', name: 'Mine'}), isLocal: true, ...over};
}

function fakeTab(over: Partial<BigTraceEditorTab> = {}): BigTraceEditorTab {
  return {
    title: 'Query 1',
    editorText: '',
    limit: MODE_DEFAULTS.ephemeral.rowLimit,
    materialize: false,
    configured: false,
    querySettings: [],
    traceFilters: [],
    traceMetadataColumns: null,
    traceOrderBy: '',
    disabledSettings: [],
    ...over,
  } as unknown as BigTraceEditorTab;
}

describe('launcher preset list', () => {
  test('backend presets come before locally saved ones', () => {
    expect(
      launcherPresets([preset({id: 'a'}), preset({id: 'b'})], [local()]).map(
        (p) => p.id,
      ),
    ).toEqual(['a', 'b', 'local:1']);
  });

  test('local presets are recognisable', () => {
    expect(isLocalPreset(local())).toBe(true);
    expect(isLocalPreset(preset())).toBe(false);
  });

  test('the last used preset is preselected while it exists', () => {
    const presets = [preset({id: 'a'}), local()];
    expect(preselectedPresetId(presets, 'local:1')).toBe('local:1');
    // Deleted since it was last used.
    expect(preselectedPresetId(presets, 'local:gone')).toBeUndefined();
    // Nothing used yet, or the last query was configured by hand.
    expect(preselectedPresetId(presets, '')).toBeUndefined();
  });
});

describe('applyPresetToTab', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  function regBool(id: string) {
    return bigTraceSettingsStorage.register({
      id,
      name: id,
      description: '',
      type: 'boolean',
      schema: z.boolean() as never,
      defaultValue: true,
      category: 'BIGTRACE_QUERY_OPTIONS',
    });
  }

  function regString(id: string) {
    return bigTraceSettingsStorage.register({
      id,
      name: id,
      description: '',
      type: 'string',
      schema: z.string() as never,
      defaultValue: '',
      category: 'TRACE_ADDRESS',
    });
  }

  test('loads the query, title, selection and marks the tab configured', () => {
    const tab = fakeTab();
    applyPresetToTab(
      tab,
      preset({
        name: 'Jank by device',
        perfettoSql: 'select * from slice',
        traceFilters: [{field: 'device_name', op: 'is not null'}],
        traceMetadataColumns: ['device_name'],
        traceOrderBy: 'size_bytes desc',
        limit: 42,
        materialized: true,
      }),
    );
    expect(tab.configured).toBe(true);
    expect(tab.title).toBe('Jank by device');
    expect(tab.editorText).toBe('select * from slice');
    expect(tab.traceFilters).toEqual([
      {field: 'device_name', op: 'is not null'},
    ]);
    expect(tab.traceMetadataColumns).toEqual(['device_name']);
    expect(tab.traceOrderBy).toBe('size_bytes desc');
    expect(tab.limit).toBe(42);
    expect(tab.materialize).toBe(true);
  });

  test('a preset without a row cap takes the default for its mode', () => {
    const persistent = fakeTab();
    applyPresetToTab(persistent, preset({materialized: true}));
    expect(persistent.limit).toBe(MODE_DEFAULTS.persistent.rowLimit);

    const ephemeral = fakeTab();
    applyPresetToTab(ephemeral, preset({materialized: false}));
    expect(ephemeral.limit).toBe(MODE_DEFAULTS.ephemeral.rowLimit);
    expect(effectiveTraceLimit(ephemeral)).toBe(
      MODE_DEFAULTS.ephemeral.traceLimit,
    );
  });

  test('settings the preset omits are turned off', () => {
    regString('trace_directory');
    regBool('treat_trace_errors_as_warning');
    const tab = fakeTab();
    applyPresetToTab(
      tab,
      preset({
        settings: [
          {
            settingId: 'trace_directory',
            values: ['/traces'],
            category: 'TRACE_ADDRESS',
          },
        ],
      }),
    );
    // Stated setting applied...
    expect(
      tab.querySettings.find((s) => s.settingId === 'trace_directory')?.values,
    ).toEqual(['/traces']);
    // ...boolean it doesn't mention forced off (booleans can't be disabled)...
    expect(
      tab.querySettings.find(
        (s) => s.settingId === 'treat_trace_errors_as_warning',
      )?.values,
    ).toEqual(['false']);
    // ...and nothing else is silently left enabled.
    expect(tab.disabledSettings).toEqual([]);
  });

  test('a togglable setting the preset omits is disabled, not blanked', () => {
    regString('trace_directory');
    regBool('a_flag');
    bigTraceSettingsStorage.register({
      id: 'other_filter',
      name: 'other_filter',
      description: '',
      type: 'string',
      schema: z.string() as never,
      defaultValue: '',
      category: 'BIGTRACE_QUERY_OPTIONS',
    });
    const tab = fakeTab();
    applyPresetToTab(
      tab,
      preset({
        settings: [
          {
            settingId: 'trace_directory',
            values: ['/traces'],
            category: 'TRACE_ADDRESS',
          },
        ],
      }),
    );
    expect(tab.disabledSettings).toEqual(['other_filter']);
  });

  test('the trace source survives applying a preset that omits it', () => {
    regString('trace_directory');
    const tab = fakeTab({
      querySettings: [
        {
          settingId: 'trace_directory',
          values: ['/my/traces'],
          category: 'TRACE_ADDRESS',
        },
      ],
    });
    // A shared catalog can't know where this user's traces live, so a preset
    // that doesn't name the source must not clear or disable it.
    applyPresetToTab(tab, preset({settings: []}));
    expect(
      tab.querySettings.find((s) => s.settingId === 'trace_directory')?.values,
    ).toEqual(['/my/traces']);
    expect(tab.disabledSettings).not.toContain('trace_directory');
  });

  test('a preset that does name the source overrides it', () => {
    regString('trace_directory');
    const tab = fakeTab({
      querySettings: [
        {
          settingId: 'trace_directory',
          values: ['/my/traces'],
          category: 'TRACE_ADDRESS',
        },
      ],
    });
    applyPresetToTab(
      tab,
      preset({
        settings: [
          {
            settingId: 'trace_directory',
            values: ['/preset/traces'],
            category: 'TRACE_ADDRESS',
          },
        ],
      }),
    );
    expect(
      tab.querySettings.find((s) => s.settingId === 'trace_directory')?.values,
    ).toEqual(['/preset/traces']);
  });

  test('empty metadata columns from the wire mean "unchosen"', () => {
    const tab = fakeTab({traceMetadataColumns: ['stale']});
    applyPresetToTab(tab, preset({traceMetadataColumns: []}));
    expect(tab.traceMetadataColumns).toBeNull();
  });
});

describe('matchingPresetId', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  test('finds the preset a tab was configured from', () => {
    const p = preset({
      id: 'jank',
      perfettoSql: 'select * from slice',
      traceOrderBy: 'size_bytes desc',
    });
    const tab = fakeTab();
    applyPresetToTab(tab, p);
    expect(matchingPresetId(tab, [preset({id: 'other'}), p])).toBe('jank');
  });

  test('an edited query no longer matches', () => {
    const p = preset({id: 'jank', perfettoSql: 'select * from slice'});
    const tab = fakeTab();
    applyPresetToTab(tab, p);
    tab.editorText = 'select * from slice limit 5';
    expect(matchingPresetId(tab, [p])).toBeUndefined();
  });
});
