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
  DEFAULT_LOCAL_CATEGORY,
  isLocalPresetId,
  localPresetStore,
  presetFromTab,
  presetNamed,
  type LocalPreset,
} from './local_preset_store';
import {lastPresetIdState} from '../settings/last_preset_state';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import type {BigTraceEditorTab} from '../pages/query_tabs_state';

function preset(over: Partial<LocalPreset> = {}): LocalPreset {
  return {
    isLocal: true,
    id: '',
    name: 'A preset',
    category: DEFAULT_LOCAL_CATEGORY,
    description: '',
    perfettoSql: 'select 1',
    ...over,
  };
}

function fakeTab(over: Partial<BigTraceEditorTab> = {}): BigTraceEditorTab {
  return {
    editorText: 'select 1',
    limit: 1000,
    materialize: false,
    querySettings: [],
    traceFilters: [],
    traceMetadataColumns: null,
    traceOrderBy: '',
    disabledSettings: [],
    ...over,
  } as unknown as BigTraceEditorTab;
}

describe('localPresetStore', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  test('save assigns a local id and lists the preset back', () => {
    const saved = localPresetStore.save(preset({name: 'Jank'}));
    expect(isLocalPresetId(saved.id)).toBe(true);
    expect(localPresetStore.list().map((p) => p.name)).toEqual(['Jank']);
    expect(localPresetStore.get(saved.id)?.isLocal).toBe(true);
  });

  test('saving an existing id overwrites in place', () => {
    const first = localPresetStore.save(preset({name: 'One'}));
    localPresetStore.save(preset({name: 'Two'}));
    localPresetStore.save(preset({id: first.id, name: 'One edited'}));
    expect(localPresetStore.list().map((p) => p.name)).toEqual([
      'One edited',
      'Two',
    ]);
  });

  test('update changes what a preset shows, not what it runs', () => {
    const p = localPresetStore.save(
      preset({name: 'Old', perfettoSql: 'select 2', limit: 7}),
    );
    localPresetStore.update(p.id, {
      name: 'New',
      category: 'Latency',
      description: 'Slow frames per trace.',
    });
    const got = localPresetStore.get(p.id)!;
    expect(got.name).toBe('New');
    expect(got.category).toBe('Latency');
    expect(got.description).toBe('Slow frames per trace.');
    expect(got.perfettoSql).toBe('select 2');
    expect(got.limit).toBe(7);
    localPresetStore.remove(p.id);
    expect(localPresetStore.list()).toEqual([]);
  });

  test('update of an unknown id is a no-op', () => {
    localPresetStore.save(preset({name: 'Kept'}));
    localPresetStore.update('local:nope', {
      name: 'Changed',
      category: '',
      description: '',
    });
    expect(localPresetStore.list().map((p) => p.name)).toEqual(['Kept']);
  });

  test('presetNamed ignores case and surrounding space', () => {
    const list = [preset({id: 'a', name: 'Startup latency'})];
    expect(presetNamed('  startup LATENCY ', list)?.id).toBe('a');
    expect(presetNamed('other', list)).toBeUndefined();
  });

  test('findByName ignores case and surrounding space', () => {
    localPresetStore.save(preset({name: 'Startup latency'}));
    expect(localPresetStore.findByName('  startup LATENCY ')?.name).toBe(
      'Startup latency',
    );
    expect(localPresetStore.findByName('other')).toBeUndefined();
  });

  test('malformed storage yields an empty list, not a crash', () => {
    localStorage.setItem('bigtraceLocalPresets', 'not json');
    expect(localPresetStore.list()).toEqual([]);
    localStorage.setItem(
      'bigtraceLocalPresets',
      JSON.stringify({presets: 'nope'}),
    );
    expect(localPresetStore.list()).toEqual([]);
  });

  test('entries missing an id, name, or SQL are dropped', () => {
    localStorage.setItem(
      'bigtraceLocalPresets',
      JSON.stringify({
        presets: [
          {id: 'local:1', name: 'ok', perfettoSql: 'select 1'},
          {id: 'local:2', name: 'no sql'},
          {name: 'no id', perfettoSql: 'select 1'},
          {id: 'local:4', name: '', perfettoSql: 'select 1'},
          null,
        ],
      }),
    );
    expect(localPresetStore.list().map((p) => p.id)).toEqual(['local:1']);
  });
});

describe('presetFromTab', () => {
  beforeEach(() => {
    localStorage.clear();
    bigTraceSettingsStorage.clear();
  });

  test('captures effective settings, selection, limit and mode', () => {
    bigTraceSettingsStorage.register({
      id: 'trace_directory',
      name: 'Trace Directory',
      description: '',
      type: 'string',
      schema: z.string(),
      defaultValue: '/global',
      category: 'TRACE_ADDRESS',
    });
    const tab = fakeTab({
      editorText: 'select count(*) from slice',
      limit: 10000,
      materialize: true,
      traceOrderBy: 'size_bytes desc',
      traceFilters: [{field: 'file_name', op: 'glob', value: '*.pftrace'}],
      querySettings: [
        {
          settingId: 'trace_limit',
          values: ['100000'],
          category: 'TRACE_ADDRESS',
        },
      ],
    });

    const p = presetFromTab(tab, {name: 'Slice count'});

    expect(p.isLocal).toBe(true);
    expect(p.name).toBe('Slice count');
    expect(p.category).toBe(DEFAULT_LOCAL_CATEGORY);
    expect(p.perfettoSql).toBe('select count(*) from slice');
    expect(p.limit).toBe(10000);
    expect(p.materialized).toBe(true);
    expect(p.traceOrderBy).toBe('size_bytes desc');
    expect(p.traceFilters).toEqual([
      {field: 'file_name', op: 'glob', value: '*.pftrace'},
    ]);
    // Global default merged with the per-tab override.
    const byId = new Map(
      (p.settings ?? []).map((s) => [s.settingId, s.values]),
    );
    expect(byId.get('trace_directory')).toEqual(['/global']);
    expect(byId.get('trace_limit')).toEqual(['100000']);
  });

  test('null metadata columns ship as the empty list', () => {
    const p = presetFromTab(fakeTab({traceMetadataColumns: null}), {
      name: 'n',
    });
    expect(p.traceMetadataColumns).toEqual([]);
  });

  test('an explicit column list is preserved', () => {
    const p = presetFromTab(fakeTab({traceMetadataColumns: ['device_name']}), {
      name: 'n',
    });
    expect(p.traceMetadataColumns).toEqual(['device_name']);
  });

  test('an explicit id overwrites an existing preset', () => {
    const saved = localPresetStore.save(preset({name: 'Old name'}));
    localPresetStore.save(
      presetFromTab(fakeTab(), {name: 'New name', id: saved.id}),
    );
    expect(localPresetStore.list()).toHaveLength(1);
    expect(localPresetStore.list()[0].name).toBe('New name');
  });
});

describe('lastPresetIdState', () => {
  beforeEach(() => localStorage.clear());

  test('last preset id round-trips, defaulting to empty', () => {
    expect(lastPresetIdState.get()).toBe('');
    lastPresetIdState.set('local:abc');
    expect(lastPresetIdState.get()).toBe('local:abc');
  });

  test('a non-string reads back as empty', () => {
    localStorage.setItem('bigtraceLastPreset', JSON.stringify({id: 42}));
    expect(lastPresetIdState.get()).toBe('');
  });
});
