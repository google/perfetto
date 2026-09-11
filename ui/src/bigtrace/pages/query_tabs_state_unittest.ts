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
  applyPresetSetup,
  closeSettings,
  isTraceSelectionSetting,
  parseTraceUuids,
  setTraceUuidsActive,
  TRACE_UUIDS_SETTING_ID,
  traceUuidsActive,
  disabledSettingsFromSnapshot,
  effectiveTabSettings,
  MODE_DEFAULTS,
  openSettings,
  QueryTabsState,
  restoreTabConfig,
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
    traceLimit: MODE_DEFAULTS.ephemeral.traceLimit,
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
    const limit = reg('cpu_filter', 100, 'number');
    const tab = fakeTab({disabledSettings: ['cpu_filter']});
    const ids = effectiveTabSettings(tab).map((s) => s.settingId);
    expect(ids).toContain('trace_directory');
    expect(ids).not.toContain('cpu_filter'); // dropped for this tab
    // Independence: the GLOBAL settings stay enabled.
    expect(limit.isDisabled()).toBe(false);
    expect(dir.isDisabled()).toBe(false);
  });

  test('a globally-disabled setting can be re-enabled per-tab', () => {
    const limit = reg('cpu_filter', 100, 'number');
    limit.setDisabled(true); // globally OFF
    const tab = fakeTab({disabledSettings: []}); // tab leaves it on
    expect(effectiveTabSettings(tab).map((s) => s.settingId)).toContain(
      'cpu_filter',
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
    reg('cpu_filter', 100, 'number');
    // A tab that turned cpu_filter OFF — this is what a run submits.
    const tab = fakeTab({disabledSettings: ['cpu_filter']});
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
    ).toEqual(['cpu_filter']);
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
    const tabs = new QueryTabsState();
    const tab = tabs.addNewTab(undefined, '', undefined, undefined, false);
    expect(tab.limit).toBe(MODE_DEFAULTS.ephemeral.rowLimit);
    expect(tab.traceLimit).toBe(MODE_DEFAULTS.ephemeral.traceLimit);
  });

  test('a new persistent tab gets the full-sweep caps', () => {
    const tabs = new QueryTabsState();
    const tab = tabs.addNewTab(undefined, '', undefined, undefined, true);
    expect(tab.limit).toBe(MODE_DEFAULTS.persistent.rowLimit);
    expect(tab.traceLimit).toBe(MODE_DEFAULTS.persistent.traceLimit);
  });

  test('caller-supplied caps win over the mode defaults', () => {
    const tabs = new QueryTabsState();
    const tab = tabs.addNewTab(
      undefined,
      '',
      42,
      undefined,
      true,
      undefined,
      undefined,
      77,
    );
    expect(tab.limit).toBe(42);
    expect(tab.traceLimit).toBe(77);
  });

  test('switching mode moves caps the user never touched', () => {
    const tab = fakeTab({materialize: false});
    applyModeDefaults(tab, true);
    expect(tab.materialize).toBe(true);
    expect(tab.limit).toBe(MODE_DEFAULTS.persistent.rowLimit);
    expect(tab.traceLimit).toBe(MODE_DEFAULTS.persistent.traceLimit);
    applyModeDefaults(tab, false);
    expect(tab.limit).toBe(MODE_DEFAULTS.ephemeral.rowLimit);
    expect(tab.traceLimit).toBe(MODE_DEFAULTS.ephemeral.traceLimit);
  });

  test('switching mode leaves hand-edited caps alone', () => {
    const tab = fakeTab({materialize: false, limit: 7});
    tab.traceLimit = 25;
    applyModeDefaults(tab, true);
    expect(tab.limit).toBe(7);
    expect(tab.traceLimit).toBe(25);
  });

  test('a cap that matches the new mode default still moves with the mode', () => {
    // Explicitly set to the ephemeral default, then switch: it reads as
    // untouched, which is the documented trade-off of the equality check.
    const tab = fakeTab({materialize: false});
    tab.traceLimit = MODE_DEFAULTS.ephemeral.traceLimit;
    applyModeDefaults(tab, true);
    expect(tab.traceLimit).toBe(MODE_DEFAULTS.persistent.traceLimit);
  });

  test('the trace cap round-trips through storage', () => {
    const tabs = new QueryTabsState();
    const tab = tabs.addNewTab(
      undefined,
      'select 1',
      undefined,
      undefined,
      true,
    );
    tab.traceLimit = 123;
    (tabs as unknown as {saveToStorage: () => void}).saveToStorage();
    const restored = new QueryTabsState().tabs.find(
      (t) => t.editorText === 'select 1',
    )!;
    expect(restored.traceLimit).toBe(123);
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
    expect(clone.traceLimit).toBe(src.traceLimit);
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
      experimentFilter: {
        experimentId: 111,
        controlId: 222,
        isTreatment: true,
        experimentName: 'an experiment',
      },
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
    tab.traceLimit = 11;
    tab.materialize = false;
    tab.experimentFilter = {
      experimentId: 999,
      controlId: 888,
      isTreatment: false,
    };
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
    expect(tab.traceLimit).toBe(11);
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

describe('isTraceSelectionSetting', () => {
  test('the source and metadata categories are selection; options are not', () => {
    expect(
      isTraceSelectionSetting({
        id: 'trace_directory',
        category: 'TRACE_ADDRESS',
      }),
    ).toBe(true);
    expect(
      isTraceSelectionSetting({
        id: 'device_filter',
        category: 'TRACE_METADATA',
      }),
    ).toBe(true);
    expect(
      isTraceSelectionSetting({
        id: 'warn',
        category: 'BIGTRACE_QUERY_OPTIONS',
      }),
    ).toBe(false);
    expect(isTraceSelectionSetting({id: 'misc'})).toBe(false);
  });
});

describe('trace UUID selection mode', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  function regUuids() {
    return bigTraceSettingsStorage.register({
      id: TRACE_UUIDS_SETTING_ID,
      name: 'Trace UUIDs',
      description: '',
      type: 'string-array',
      schema: z.array(z.string()) as never,
      defaultValue: [] as string[],
      category: 'TRACE_ADDRESS',
    });
  }

  test('parseTraceUuids splits on commas and whitespace, dedupes', () => {
    expect(parseTraceUuids('a, b\n c,,a\t d ')).toEqual(['a', 'b', 'c', 'd']);
    expect(parseTraceUuids('')).toEqual([]);
    expect(parseTraceUuids(' ,\n, ')).toEqual([]);
  });

  test('the mode needs the declared setting AND an explicit tab entry', () => {
    const tab = fakeTab({disabledSettings: []});
    expect(traceUuidsActive(tab)).toBe(false);
    setTraceUuidsActive(tab, true); // no-op: not declared
    expect(tab.disabledSettings).toEqual([]);
    expect(tab.querySettings).toEqual([]);
    regUuids();
    // Declared but no tab entry — a fresh tab created before the config
    // arrived stays out of the mode.
    expect(traceUuidsActive(tab)).toBe(false);
    setTraceUuidsActive(tab, true);
    expect(traceUuidsActive(tab)).toBe(true);
    expect(
      tab.querySettings.find((s) => s.settingId === TRACE_UUIDS_SETTING_ID)
        ?.values,
    ).toEqual([]);
  });

  test('toggling the mode is non-destructive, both ways', () => {
    regUuids();
    const tab = fakeTab({
      disabledSettings: [TRACE_UUIDS_SETTING_ID],
      traceFilters: [{field: 'file_name', op: 'glob', value: '*.pftrace'}],
      querySettings: [
        {
          settingId: 'trace_directory',
          values: ['/traces'],
          category: 'TRACE_ADDRESS',
        },
        {
          settingId: TRACE_UUIDS_SETTING_ID,
          values: ['u1'],
          category: 'TRACE_ADDRESS',
        },
      ],
    });
    expect(traceUuidsActive(tab)).toBe(false);
    setTraceUuidsActive(tab, true);
    expect(traceUuidsActive(tab)).toBe(true);
    // Filter-mode configuration is hidden, not cleared...
    expect(tab.traceFilters).toHaveLength(1);
    expect(tab.querySettings[0].values).toEqual(['/traces']);
    // ...and an earlier stint's values survive re-entry.
    expect(
      tab.querySettings.find((s) => s.settingId === TRACE_UUIDS_SETTING_ID)
        ?.values,
    ).toEqual(['u1']);
    setTraceUuidsActive(tab, false);
    expect(traceUuidsActive(tab)).toBe(false);
    expect(tab.disabledSettings).toEqual([TRACE_UUIDS_SETTING_ID]);
  });

  test('a preset that does not name the list turns the mode off', () => {
    regUuids();
    const tab = fakeTab({disabledSettings: []});
    setTraceUuidsActive(tab, true);
    expect(traceUuidsActive(tab)).toBe(true);
    applyPresetSetup(tab, {
      id: 'p',
      category: 'A',
      name: 'P',
      description: '',
      perfettoSql: 'select 1',
    });
    expect(traceUuidsActive(tab)).toBe(false);
  });

  test('a preset that names the list turns the mode on with its values', () => {
    regUuids();
    const tab = fakeTab({disabledSettings: [TRACE_UUIDS_SETTING_ID]});
    applyPresetSetup(tab, {
      id: 'p',
      category: 'A',
      name: 'P',
      description: '',
      perfettoSql: '',
      settings: [
        {
          settingId: TRACE_UUIDS_SETTING_ID,
          values: ['u1', 'u2'],
          category: 'TRACE_ADDRESS',
        },
      ],
    });
    expect(traceUuidsActive(tab)).toBe(true);
    expect(
      tab.querySettings.find((s) => s.settingId === TRACE_UUIDS_SETTING_ID)
        ?.values,
    ).toEqual(['u1', 'u2']);
  });
});

describe('experiment filter', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  const chosen = {
    experimentId: 111,
    controlId: 222,
    isTreatment: true,
    experimentName: 'an experiment',
    controlName: 'its control',
  };

  test('Cancel puts back the experiment the query had', () => {
    const tab = fakeTab({configured: true, experimentFilter: {...chosen}});
    openSettings(tab);
    tab.experimentFilter = {
      experimentId: 999,
      controlId: 888,
      isTreatment: false,
    };
    closeSettings(tab, {keep: false});
    expect(tab.experimentFilter).toEqual(chosen);
  });

  test('Cancel puts back one that was cleared', () => {
    const tab = fakeTab({configured: true, experimentFilter: {...chosen}});
    openSettings(tab);
    tab.experimentFilter = undefined;
    closeSettings(tab, {keep: false});
    expect(tab.experimentFilter).toEqual(chosen);
  });

  test('Cancel takes away one that was picked', () => {
    const tab = fakeTab({configured: true});
    openSettings(tab);
    tab.experimentFilter = {...chosen};
    closeSettings(tab, {keep: false});
    expect(tab.experimentFilter).toBeUndefined();
  });

  test('Apply keeps the switch of arm', () => {
    const tab = fakeTab({configured: true, experimentFilter: {...chosen}});
    openSettings(tab);
    tab.experimentFilter = {...chosen, isTreatment: false};
    closeSettings(tab, {keep: true});
    expect(tab.experimentFilter?.isTreatment).toBe(false);
  });

  test('the snapshot is a copy: later edits do not reach it', () => {
    const tab = fakeTab({experimentFilter: {...chosen}});
    const snap = snapshotTabConfig(tab);
    tab.experimentFilter = {...chosen, isTreatment: false};
    expect(snap.experimentFilter?.isTreatment).toBe(true);
    restoreTabConfig(tab, snap);
    // Restoring hands out a copy too.
    tab.experimentFilter = {...tab.experimentFilter!, isTreatment: false};
    expect(snap.experimentFilter?.isTreatment).toBe(true);
  });

  test('a clone runs over the same experiment, in its own object', () => {
    const tabs = new QueryTabsState();
    const src = tabs.addNewTab(undefined, 'select 1');
    src.configured = true;
    src.experimentFilter = {...chosen};
    const clone = tabs.cloneTab(src.id);
    expect(clone?.experimentFilter).toEqual(chosen);
    expect(clone?.experimentFilter).not.toBe(src.experimentFilter);
  });

  test('the experiment round-trips through storage, names and all', () => {
    const tabs = new QueryTabsState();
    const tab = tabs.addNewTab(undefined, 'select 1');
    tab.configured = true;
    tab.experimentFilter = {...chosen};
    (tabs as unknown as {saveToStorage: () => void}).saveToStorage();

    const reloaded = new QueryTabsState().tabs.find(
      (t) => t.editorText === 'select 1',
    );
    expect(reloaded?.experimentFilter).toEqual(chosen);
  });

  test('a new query starts with no experiment', () => {
    const tabs = new QueryTabsState();
    expect(tabs.addNewTab().experimentFilter).toBeUndefined();
  });

  test('tabs stored before experiments existed still load', () => {
    localStorage.setItem(
      'bigtraceQueryTabs',
      JSON.stringify({
        tabs: [
          {
            id: 'a',
            title: 'Query 1',
            editorText: 'select 1',
            limit: 1000,
            materialize: true,
            configured: true,
          },
        ],
        activeTabId: 'a',
      }),
    );
    const tabs = new QueryTabsState();
    expect(tabs.tabs).toHaveLength(1);
    expect(tabs.tabs[0].experimentFilter).toBeUndefined();
  });

  test('a preset naming an experiment runs the query over it', () => {
    const tab = fakeTab({});
    applyPresetSetup(tab, {
      id: 'p',
      category: '',
      name: 'p',
      description: '',
      perfettoSql: 'select 1',
      experimentFilter: {experimentId: 1, controlId: 2, isTreatment: false},
    });
    expect(tab.experimentFilter).toEqual({
      experimentId: 1,
      controlId: 2,
      isTreatment: false,
    });
  });

  test('a preset that names none turns the experiment off', () => {
    const tab = fakeTab({experimentFilter: {...chosen}});
    applyPresetSetup(tab, {
      id: 'p',
      category: '',
      name: 'p',
      description: '',
      perfettoSql: 'select 1',
    });
    expect(tab.experimentFilter).toBeUndefined();
  });

  test('applying replaces wholesale: no names from the one before', () => {
    const tab = fakeTab({experimentFilter: {...chosen}});
    applyPresetSetup(tab, {
      id: 'p',
      category: '',
      name: 'p',
      description: '',
      perfettoSql: 'select 1',
      experimentFilter: {experimentId: 333, controlId: 444, isTreatment: true},
    });
    expect(tab.experimentFilter).toEqual({
      experimentId: 333,
      controlId: 444,
      isTreatment: true,
    });
    expect(tab.experimentFilter?.experimentName).toBeUndefined();
  });
});
