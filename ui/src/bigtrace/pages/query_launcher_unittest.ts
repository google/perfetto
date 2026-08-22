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
  launcherPresets,
  matchingSetupPresetId,
  preselectedPresetId,
  selectedPresetId,
} from './query_launcher';
import {presetNameConflict} from './preset_dialogs';
import {
  applyPresetSetup,
  applyPresetToTab,
  effectiveTabSettings,
  effectiveTraceLimit,
  MODE_DEFAULTS,
  openSettings,
  setTraceLimit,
  traceLimitDisabled,
  type BigTraceEditorTab,
} from './query_tabs_state';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import type {TracePreset} from '../query/bigtrace_query_client';
import {isLocalPreset, type LocalPreset} from '../query/local_preset_store';

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

describe('applyPresetToTab', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  test("fills the query, title and selection; leaving the launcher is the caller's", () => {
    const tab = fakeTab({configured: false});
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
    expect(tab.configured).toBe(false);
    // Tabs keep their own name — "Query N" until the user renames one.
    expect(tab.title).toBe('Query 1');
    expect(tab.editorText).toBe('select * from slice');
    expect(tab.traceFilters).toEqual([
      {field: 'device_name', op: 'is not null'},
    ]);
    expect(tab.traceMetadataColumns).toEqual(['device_name']);
    expect(tab.traceOrderBy).toBe('size_bytes desc');
    expect(tab.limit).toBe(42);
    expect(tab.materialize).toBe(true);
  });

  test('applyPresetSetup takes everything but the query', () => {
    const tab = fakeTab({
      configured: true,
      title: 'Slice count',
      editorText: 'select count(*) from slice',
      traceOrderBy: 'mtime asc',
    });
    openSettings(tab);
    applyPresetSetup(
      tab,
      preset({
        name: 'Fleet sweep',
        perfettoSql: 'select * from thread',
        traceFilters: [{field: 'device_name', op: 'is not null'}],
        traceMetadataColumns: ['device_name'],
        traceOrderBy: 'size_bytes desc',
        limit: 42,
        materialized: true,
      }),
    );
    // The query and its title are the user's.
    expect(tab.editorText).toBe('select count(*) from slice');
    expect(tab.title).toBe('Slice count');
    // The setup is the preset's, caps and mode included.
    expect(tab.traceFilters).toEqual([
      {field: 'device_name', op: 'is not null'},
    ]);
    expect(tab.traceMetadataColumns).toEqual(['device_name']);
    expect(tab.traceOrderBy).toBe('size_bytes desc');
    expect(tab.limit).toBe(42);
    expect(tab.materialize).toBe(true);
    // Still inside Settings: provisional until Apply.
    expect(tab.settingsSession).toBeDefined();
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

  test('a preset describes the whole run: what it omits is turned off', () => {
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
    // Nothing carries over from what the tab happened to hold before.
    applyPresetToTab(tab, preset({settings: []}));
    expect(tab.disabledSettings).toContain('trace_directory');
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

  test('a preset that omits the trace cap still ships one', () => {
    bigTraceSettingsStorage.register({
      id: 'trace_limit',
      name: 'trace_limit',
      description: '',
      type: 'number',
      schema: z.number() as never,
      defaultValue: 100,
      category: 'TRACE_ADDRESS',
    });
    const tab = fakeTab();
    // The reference catalog's presets don't mention the cap. Disabling it
    // would run the query over every trace in the corpus while the toolbar
    // showed a number.
    applyPresetToTab(tab, preset({materialized: true, settings: []}));
    expect(traceLimitDisabled(tab)).toBe(false);
    expect(effectiveTraceLimit(tab)).toBe(MODE_DEFAULTS.persistent.traceLimit);
    expect(
      effectiveTabSettings(tab).find((s) => s.settingId === 'trace_limit')
        ?.values,
    ).toEqual([String(MODE_DEFAULTS.persistent.traceLimit)]);
  });

  test('a preset that states a trace cap keeps it', () => {
    bigTraceSettingsStorage.register({
      id: 'trace_limit',
      name: 'trace_limit',
      description: '',
      type: 'number',
      schema: z.number() as never,
      defaultValue: 100,
      category: 'TRACE_ADDRESS',
    });
    const tab = fakeTab();
    applyPresetToTab(
      tab,
      preset({
        settings: [
          {settingId: 'trace_limit', values: ['25'], category: 'TRACE_ADDRESS'},
        ],
      }),
    );
    expect(effectiveTraceLimit(tab)).toBe(25);
  });

  test('setting a cap re-enables one the tab had switched off', () => {
    bigTraceSettingsStorage.register({
      id: 'trace_limit',
      name: 'trace_limit',
      description: '',
      type: 'number',
      schema: z.number() as never,
      defaultValue: 100,
      category: 'TRACE_ADDRESS',
    });
    const tab = fakeTab({disabledSettings: ['trace_limit']});
    // Uncapped until the user types a number in the toolbar.
    expect(traceLimitDisabled(tab)).toBe(true);
    expect(effectiveTabSettings(tab).map((s) => s.settingId)).not.toContain(
      'trace_limit',
    );
    setTraceLimit(tab, 50);
    expect(traceLimitDisabled(tab)).toBe(false);
    expect(
      effectiveTabSettings(tab).find((s) => s.settingId === 'trace_limit')
        ?.values,
    ).toEqual(['50']);
  });

  test('empty metadata columns from the wire mean "unchosen"', () => {
    const tab = fakeTab({traceMetadataColumns: ['stale']});
    applyPresetToTab(tab, preset({traceMetadataColumns: []}));
    expect(tab.traceMetadataColumns).toBeNull();
  });
});

describe('selectedPresetId', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
    regString('trace_directory');
  });

  test('reads back the preset a tab was filled from', () => {
    const p = preset({
      id: 'jank',
      perfettoSql: 'select * from slice',
      traceOrderBy: 'size_bytes desc',
    });
    const tab = fakeTab();
    applyPresetToTab(tab, p);
    expect(selectedPresetId(tab, [preset({id: 'other'}), p])).toBe('jank');
  });

  test("an edited query, or a setup with extras, is nobody's", () => {
    const p = preset({id: 'jank', perfettoSql: 'select * from slice'});
    const tab = fakeTab();
    applyPresetToTab(tab, p);
    tab.editorText = 'select * from slice limit 5';
    expect(selectedPresetId(tab, [p])).toBeUndefined();
    tab.editorText = 'select * from slice';
    tab.disabledSettings = [];
    expect(selectedPresetId(tab, [p])).toBeUndefined();
  });

  test("a fresh tab is nobody's, whatever the catalog holds", () => {
    // A query-only preset and a setup-only one: neither fits a tab with every
    // setting on at its default.
    const tab = fakeTab({editorText: ''});
    expect(
      selectedPresetId(tab, [
        preset({id: 'q', perfettoSql: 'select 1'}),
        preset({id: 's', perfettoSql: ''}),
      ]),
    ).toBeUndefined();
  });

  test('among presets that fit alike, the one last applied wins', () => {
    const a = preset({id: 'a', perfettoSql: 'select 1'});
    const b = preset({id: 'b', perfettoSql: 'select 1'});
    const tab = fakeTab();
    applyPresetToTab(tab, b);
    expect(selectedPresetId(tab, [a, b])).toBe('a');
    expect(selectedPresetId(tab, [a, b], tab.lastPresetId)).toBe('b');
  });
});

describe('matchingSetupPresetId', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
    regString('trace_directory');
    regBool('warn');
  });

  const sweep = preset({
    id: 'sweep',
    perfettoSql: 'select 1',
    settings: [
      {
        settingId: 'trace_directory',
        values: ['/traces'],
        category: 'TRACE_ADDRESS',
      },
    ],
  });
  // Query-only catalog entries: identical (empty) setups.
  const lmk = preset({id: 'lmk', perfettoSql: 'select 2'});
  const oom = preset({id: 'oom', perfettoSql: 'select 3'});

  test('a hand-made setup reads as Custom, whatever the query', () => {
    // Fresh tab: every registered setting on at its default.
    const tab = fakeTab({editorText: 'select 1'});
    expect(matchingSetupPresetId(tab, [sweep, lmk, oom])).toBeUndefined();
  });

  test('after applying a preset the tab reads as that preset, query aside', () => {
    const tab = fakeTab({editorText: 'select something_else'});
    applyPresetSetup(tab, sweep);
    expect(matchingSetupPresetId(tab, [lmk, sweep])).toBe('sweep');
  });

  test('a setting the preset left off, switched on, makes it Custom', () => {
    const tab = fakeTab();
    applyPresetSetup(tab, lmk);
    expect(matchingSetupPresetId(tab, [lmk])).toBe('lmk');
    tab.disabledSettings = tab.disabledSettings.filter(
      (id) => id !== 'trace_directory',
    );
    expect(matchingSetupPresetId(tab, [lmk])).toBeUndefined();
  });

  test('among identical setups the one applied is read back', () => {
    const tab = fakeTab();
    applyPresetSetup(tab, oom);
    // Applying records the preset; without the hint the first twin wins.
    expect(tab.lastPresetId).toBe('oom');
    expect(matchingSetupPresetId(tab, [lmk, oom])).toBe('lmk');
    expect(matchingSetupPresetId(tab, [lmk, oom], tab.lastPresetId)).toBe(
      'oom',
    );
    // ...until the setup stops being that setup.
    applyPresetSetup(tab, sweep);
    expect(matchingSetupPresetId(tab, [lmk, oom, sweep], 'oom')).toBe('sweep');
  });
});

describe('presetNameConflict', () => {
  const catalog = [preset({id: 'jank', name: 'Jank by device'})];
  const mine = local({id: 'local:1', name: 'My sweep'});
  const other = local({id: 'local:2', name: 'Other'});

  test('a catalog name is reserved, whatever the case or spacing', () => {
    const c = presetNameConflict('  jank BY device ', catalog, [mine]);
    expect(c?.kind).toBe('catalog');
    expect(c?.preset.id).toBe('jank');
  });

  test('a local name is reported as local, for the caller to decide', () => {
    const c = presetNameConflict('my sweep', catalog, [mine]);
    expect(c?.kind).toBe('local');
    expect(c?.preset.id).toBe('local:1');
  });

  test('a preset keeps its own name while being edited', () => {
    expect(
      presetNameConflict('My sweep', catalog, [mine], mine),
    ).toBeUndefined();
    expect(
      presetNameConflict('MY SWEEP', catalog, [mine], mine),
    ).toBeUndefined();
  });

  test("editing into another preset's name still conflicts", () => {
    expect(
      presetNameConflict('Other', catalog, [mine, other], mine)?.kind,
    ).toBe('local');
    expect(
      presetNameConflict('Jank by device', catalog, [mine], mine)?.kind,
    ).toBe('catalog');
  });

  test('a preset whose name already shadows the catalog can keep it', () => {
    const twin = local({id: 'local:3', name: 'Jank by device'});
    expect(
      presetNameConflict('Jank by device', catalog, [twin], twin),
    ).toBeUndefined();
  });

  test('a fresh name is free', () => {
    expect(presetNameConflict('Brand new', catalog, [mine])).toBeUndefined();
  });
});
