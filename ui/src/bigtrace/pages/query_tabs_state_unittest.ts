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
  applyModeDefaults,
  closeSettings,
  disabledSettingsFromSnapshot,
  effectiveTabSettings,
  effectiveTraceLimit,
  MODE_DEFAULTS,
  openSettings,
  QueryTabsState,
  restoreTabConfig,
  setTraceLimit,
  snapshotTabConfig,
  type BigTraceEditorTab,
} from './query_tabs_state';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';

function reg(
  id: string,
  defaultValue: unknown,
  type: 'string' | 'number',
  bounds: {min?: number; max?: number} = {},
) {
  return bigTraceSettingsStorage.register({
    id,
    name: id,
    description: '',
    type,
    schema: (type === 'number' ? z.number() : z.string()) as never,
    defaultValue,
    category: 'TRACE_ADDRESS',
    ...bounds,
  });
}

function fakeTab(over: Partial<BigTraceEditorTab>): BigTraceEditorTab {
  return {
    limit: MODE_DEFAULTS.ephemeral.rowLimit,
    materialize: false,
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

describe('per-mode row and trace limits', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  test('a new ephemeral tab gets the quick-look caps', () => {
    reg('trace_limit', 100, 'number');
    const tabs = new QueryTabsState();
    const tab = tabs.addNewTab(undefined, '', undefined, undefined, false);
    expect(tab.limit).toBe(MODE_DEFAULTS.ephemeral.rowLimit);
    expect(effectiveTraceLimit(tab)).toBe(MODE_DEFAULTS.ephemeral.traceLimit);
  });

  test('a new persistent tab gets the full-sweep caps', () => {
    reg('trace_limit', 100, 'number');
    const tabs = new QueryTabsState();
    const tab = tabs.addNewTab(undefined, '', undefined, undefined, true);
    expect(tab.limit).toBe(MODE_DEFAULTS.persistent.rowLimit);
    expect(effectiveTraceLimit(tab)).toBe(MODE_DEFAULTS.persistent.traceLimit);
  });

  test('a caller-supplied row cap wins over the mode default', () => {
    const tabs = new QueryTabsState();
    const tab = tabs.addNewTab(undefined, '', 42, undefined, true);
    expect(tab.limit).toBe(42);
  });

  test('the run ships the mode cap when the tab sets none', () => {
    reg('trace_limit', 100, 'number');
    const settings = effectiveTabSettings(fakeTab({materialize: true}));
    expect(settings.find((s) => s.settingId === 'trace_limit')?.values).toEqual(
      [String(MODE_DEFAULTS.persistent.traceLimit)],
    );
  });

  test('a trace cap disabled on the tab is not shipped at all', () => {
    reg('trace_limit', 100, 'number');
    const settings = effectiveTabSettings(
      fakeTab({materialize: true, disabledSettings: ['trace_limit']}),
    );
    expect(settings.map((s) => s.settingId)).not.toContain('trace_limit');
  });

  test('switching mode moves caps the user never touched', () => {
    reg('trace_limit', 100, 'number');
    const tab = fakeTab({materialize: false});
    applyModeDefaults(tab, true);
    expect(tab.materialize).toBe(true);
    expect(tab.limit).toBe(MODE_DEFAULTS.persistent.rowLimit);
    expect(effectiveTraceLimit(tab)).toBe(MODE_DEFAULTS.persistent.traceLimit);
    applyModeDefaults(tab, false);
    expect(tab.limit).toBe(MODE_DEFAULTS.ephemeral.rowLimit);
    expect(effectiveTraceLimit(tab)).toBe(MODE_DEFAULTS.ephemeral.traceLimit);
  });

  test('switching mode leaves hand-edited caps alone', () => {
    reg('trace_limit', 100, 'number');
    const tab = fakeTab({materialize: false, limit: 7});
    setTraceLimit(tab, 25);
    applyModeDefaults(tab, true);
    expect(tab.limit).toBe(7);
    expect(effectiveTraceLimit(tab)).toBe(25);
  });

  test('a cap that matches the new mode default still moves with the mode', () => {
    reg('trace_limit', 100, 'number');
    // Explicitly set to the ephemeral default, then switch: it reads as
    // untouched, which is the documented trade-off of the equality check.
    const tab = fakeTab({materialize: false});
    setTraceLimit(tab, MODE_DEFAULTS.ephemeral.traceLimit);
    applyModeDefaults(tab, true);
    expect(effectiveTraceLimit(tab)).toBe(MODE_DEFAULTS.persistent.traceLimit);
  });

  test('caps are clamped to the bounds the backend declared', () => {
    reg('trace_limit', 100, 'number', {min: 1, max: 10000});
    const tab = fakeTab({materialize: true});
    // The persistent default (100k) exceeds this backend's ceiling.
    expect(effectiveTraceLimit(tab)).toBe(10000);
    setTraceLimit(tab, 999999);
    expect(effectiveTraceLimit(tab)).toBe(10000);
    setTraceLimit(tab, 0);
    expect(effectiveTraceLimit(tab)).toBe(1);
  });

  test('with no trace_limit setting registered nothing is shipped or set', () => {
    const tab = fakeTab({materialize: true});
    setTraceLimit(tab, 500);
    expect(tab.querySettings).toEqual([]);
    expect(effectiveTabSettings(tab)).toEqual([]);
  });
});

describe('cloneTab', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  function configuredTab(tabs: QueryTabsState) {
    const tab = tabs.addNewTab(undefined, '', undefined, undefined, true);
    tab.title = 'Jank by device';
    tab.editorText = 'select * from slice';
    tab.configured = true;
    tab.queryUuid = 'uuid-1';
    tab.traceFilters = [{field: 'file_name', op: 'glob', value: '*.pftrace'}];
    tab.traceMetadataColumns = ['device_name'];
    tab.traceOrderBy = 'size_bytes desc';
    tab.querySettings = [
      {
        settingId: 'trace_directory',
        values: ['/traces'],
        category: 'TRACE_ADDRESS',
      },
    ];
    tab.disabledSettings = ['some_filter'];
    return tab;
  }

  test('copies the query and its configuration', () => {
    const tabs = new QueryTabsState();
    const src = configuredTab(tabs);
    const clone = tabs.cloneTab(src.id)!;

    expect(clone.id).not.toBe(src.id);
    expect(clone.editorText).toBe('select * from slice');
    expect(clone.limit).toBe(src.limit);
    expect(clone.materialize).toBe(true);
    expect(clone.configured).toBe(true);
    expect(clone.traceFilters).toEqual(src.traceFilters);
    expect(clone.traceMetadataColumns).toEqual(['device_name']);
    expect(clone.traceOrderBy).toBe('size_bytes desc');
    expect(clone.querySettings).toEqual(src.querySettings);
    expect(clone.disabledSettings).toEqual(['some_filter']);
  });

  test('the clone is unrun: no queryUuid, no results', () => {
    const tabs = new QueryTabsState();
    const src = configuredTab(tabs);
    const clone = tabs.cloneTab(src.id)!;
    // Sharing the uuid would make the clone adopt the original's execution and
    // reactivate its tab from History.
    expect(clone.queryUuid).toBeUndefined();
    expect(clone.queryResult).toBeUndefined();
    expect(clone.dataSource).toBeUndefined();
    expect(clone.isLoading).toBe(false);
  });

  test('a clone is a new tab, named like one', () => {
    const tabs = new QueryTabsState();
    const src = configuredTab(tabs);
    expect(tabs.cloneTab(src.id)?.title).toMatch(/^Query \d+$/);
  });

  test('editing the clone leaves the original alone', () => {
    const tabs = new QueryTabsState();
    const src = configuredTab(tabs);
    const clone = tabs.cloneTab(src.id)!;
    clone.traceFilters = [...clone.traceFilters, {field: 'x', op: 'is null'}];
    clone.querySettings = [
      {
        settingId: 'trace_directory',
        values: ['/other'],
        category: 'TRACE_ADDRESS',
      },
    ];
    expect(src.traceFilters).toHaveLength(1);
    expect(src.querySettings[0].values).toEqual(['/traces']);
  });

  test('an unknown id is a no-op', () => {
    const tabs = new QueryTabsState();
    const before = tabs.tabs.length;
    expect(tabs.cloneTab('nope')).toBeUndefined();
    expect(tabs.tabs).toHaveLength(before);
  });

  test('the last applied preset travels with the clone and survives reload', () => {
    const tabs = new QueryTabsState();
    const src = configuredTab(tabs);
    src.lastPresetId = 'local:sweep';
    expect(tabs.cloneTab(src.id)?.lastPresetId).toBe('local:sweep');
    (tabs as unknown as {saveToStorage: () => void}).saveToStorage();
    const restored = new QueryTabsState().tabs.find(
      (t) => t.title === 'Jank by device',
    )!;
    expect(restored.lastPresetId).toBe('local:sweep');
  });
});

describe('Settings session (Cancel restores, Apply keeps)', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  function workingTab(): BigTraceEditorTab {
    return fakeTab({
      configured: true,
      editorText: 'select 1',
      materialize: true,
      limit: 500,
      traceFilters: [{field: 'file_name', op: 'glob', value: '*.pftrace'}],
      traceMetadataColumns: ['device_name'],
      traceOrderBy: 'size_bytes desc',
      querySettings: [
        {
          settingId: 'trace_directory',
          values: ['/traces'],
          category: 'TRACE_ADDRESS',
        },
      ],
      disabledSettings: ['some_filter'],
    });
  }

  function edit(tab: BigTraceEditorTab) {
    tab.querySettings = [
      {
        settingId: 'trace_directory',
        values: ['/other'],
        category: 'TRACE_ADDRESS',
      },
    ];
    tab.disabledSettings = [];
    tab.traceFilters = [];
    tab.traceMetadataColumns = null;
    tab.traceOrderBy = '';
    tab.limit = 10;
    tab.materialize = false;
  }

  test('opening keeps the tab configured and starts a session', () => {
    const tab = workingTab();
    openSettings(tab);
    expect(tab.configured).toBe(true);
    expect(tab.settingsSession).toBeDefined();
  });

  test('Cancel puts every edited field back', () => {
    const tab = workingTab();
    const before = snapshotTabConfig(tab);
    openSettings(tab);
    edit(tab);
    closeSettings(tab, {keep: false});
    expect(snapshotTabConfig(tab)).toEqual(before);
    expect(tab.settingsSession).toBeUndefined();
    expect(tab.configured).toBe(true);
  });

  test('Apply keeps the edits', () => {
    const tab = workingTab();
    openSettings(tab);
    edit(tab);
    closeSettings(tab, {keep: true});
    expect(tab.querySettings[0].values).toEqual(['/other']);
    expect(tab.traceFilters).toEqual([]);
    expect(tab.limit).toBe(10);
    expect(tab.settingsSession).toBeUndefined();
    expect(tab.configured).toBe(true);
  });

  test('the snapshot is a copy: later mutation of the tab does not reach it', () => {
    const tab = workingTab();
    openSettings(tab);
    tab.querySettings[0].values.push('/extra');
    (tab.traceFilters as unknown[]).push({field: 'x', op: 'is null'});
    closeSettings(tab, {keep: false});
    expect(tab.querySettings[0].values).toEqual(['/traces']);
    expect(tab.traceFilters).toHaveLength(1);
  });

  test('restore hands out copies too, so the snapshot survives further edits', () => {
    const tab = workingTab();
    const snap = snapshotTabConfig(tab);
    restoreTabConfig(tab, snap);
    tab.querySettings[0].values.push('/extra');
    expect(snap.querySettings[0].values).toEqual(['/traces']);
  });

  test('a new tab starting its first query has no session and keeps its setup', () => {
    const tab = fakeTab({configured: false, editorText: ''});
    edit(tab);
    closeSettings(tab, {keep: false});
    // Nothing to restore: the tab was never configured before this.
    expect(tab.limit).toBe(10);
    expect(tab.configured).toBe(true);
  });

  test('the session is not persisted: a reload closes Settings with edits kept', () => {
    const tabs = new QueryTabsState();
    const tab = tabs.addNewTab(
      undefined,
      'select 1',
      undefined,
      undefined,
      true,
    );
    tab.configured = true;
    openSettings(tab);
    tab.limit = 42;
    // Force the debounced save.
    (tabs as unknown as {saveToStorage: () => void}).saveToStorage();
    const restored = new QueryTabsState().tabs.find(
      (t) => t.editorText === 'select 1',
    )!;
    expect(restored.configured).toBe(true);
    expect(restored.settingsSession).toBeUndefined();
    expect(restored.limit).toBe(42);
  });
});
