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
  presetMatches,
  setupEquals,
  setupMatches,
  type PresetComparable,
} from './preset_match';
import type {TracePreset} from './bigtrace_query_client';
import type {SettingFilter} from '../settings/settings_types';

function preset(over: Partial<TracePreset> = {}): TracePreset {
  return {
    id: 'p',
    category: 'Android',
    name: 'A preset',
    description: '',
    perfettoSql: 'select 1',
    ...over,
  };
}

function current(over: Partial<PresetComparable> = {}): PresetComparable {
  return {
    sql: 'select 1',
    traceFilters: [],
    traceMetadataColumns: null,
    traceOrderBy: '',
    settings: [],
    ...over,
  };
}

describe('presetMatches', () => {
  test('matches on identical SQL and empty selection', () => {
    expect(presetMatches(preset(), current())).toBe(true);
  });

  test('ignores surrounding whitespace in the SQL', () => {
    expect(presetMatches(preset(), current({sql: '  select 1\n'}))).toBe(true);
  });

  test('a setup-only preset says nothing about the query', () => {
    const p = preset({perfettoSql: ''});
    expect(presetMatches(p, current({sql: 'select 42'}))).toBe(true);
    expect(presetMatches(p, current({sql: ''}))).toBe(true);
    expect(presetMatches(p, current({traceOrderBy: 'x asc'}))).toBe(false);
  });

  test('setupMatches ignores the query entirely', () => {
    const p = preset({
      perfettoSql: 'select 1',
      traceOrderBy: 'size_bytes desc',
    });
    expect(setupMatches(p, current({traceOrderBy: 'size_bytes desc'}))).toBe(
      true,
    );
    expect(
      setupMatches(p, current({sql: 'other', traceOrderBy: 'size_bytes desc'})),
    ).toBe(true);
    expect(setupMatches(p, current())).toBe(false);
  });

  test('different SQL does not match', () => {
    expect(presetMatches(preset(), current({sql: 'select 2'}))).toBe(false);
  });

  test('filters match regardless of key order', () => {
    const p = preset({
      traceFilters: [{op: 'glob', field: 'file_name', value: '*.pftrace'}],
    });
    const c = current({
      traceFilters: [{field: 'file_name', op: 'glob', value: '*.pftrace'}],
    });
    expect(presetMatches(p, c)).toBe(true);
  });

  test('a differing filter does not match', () => {
    const p = preset({
      traceFilters: [{field: 'file_name', op: 'glob', value: '*.pftrace'}],
    });
    expect(presetMatches(p, current())).toBe(false);
  });

  test('unchosen columns (null) differ from an explicit empty list', () => {
    // The preset says nothing about columns → null; the tab picked none → [].
    expect(presetMatches(preset(), current({traceMetadataColumns: []}))).toBe(
      false,
    );
  });

  test('an explicit column list must be equal', () => {
    const p = preset({traceMetadataColumns: ['device_name']});
    expect(
      presetMatches(p, current({traceMetadataColumns: ['device_name']})),
    ).toBe(true);
    expect(
      presetMatches(p, current({traceMetadataColumns: ['android_id']})),
    ).toBe(false);
  });

  test('every setting the preset states must be effective', () => {
    const p = preset({
      settings: [
        {settingId: 'trace_directory', values: ['/traces'], category: 'X'},
      ],
    });
    expect(
      presetMatches(
        p,
        current({
          settings: [
            {
              settingId: 'trace_directory',
              values: ['/traces'],
              category: 'TRACE_ADDRESS',
            },
            {settingId: 'other', values: ['1'], category: 'TRACE_ADDRESS'},
          ],
        }),
      ),
    ).toBe(true);
    expect(
      presetMatches(
        p,
        current({
          settings: [
            {
              settingId: 'trace_directory',
              values: ['/elsewhere'],
              category: 'TRACE_ADDRESS',
            },
          ],
        }),
      ),
    ).toBe(false);
    // Missing entirely.
    expect(presetMatches(p, current())).toBe(false);
  });

  test('order_by must be equal, treating absent as empty', () => {
    expect(presetMatches(preset(), current({traceOrderBy: 'a asc'}))).toBe(
      false,
    );
    expect(
      presetMatches(
        preset({traceOrderBy: 'a asc'}),
        current({traceOrderBy: 'a asc'}),
      ),
    ).toBe(true);
  });
});

describe('setupEquals', () => {
  const kinds = {
    booleanIds: new Set(['warn']),
    ignoredIds: new Set(['trace_limit']),
  };
  const sf = (
    settingId: string,
    value: string,
    category: SettingFilter['category'],
  ): SettingFilter => ({settingId, values: [value], category});
  const dir = sf('trace_directory', '/t', 'TRACE_ADDRESS');
  const warnOff = sf('warn', 'false', 'BIGTRACE_QUERY_OPTIONS');
  const warnOn = sf('warn', 'true', 'BIGTRACE_QUERY_OPTIONS');
  const cap = sf('trace_limit', '500', 'TRACE_ADDRESS');

  test('named settings at their values, everything else off', () => {
    const p = preset({settings: [dir]});
    expect(
      setupEquals(p, current({settings: [dir, warnOff, cap]}), kinds),
    ).toBe(true);
  });

  test('an unnamed setting that is on is not that setup', () => {
    const p = preset({settings: []});
    expect(setupEquals(p, current({settings: [warnOff]}), kinds)).toBe(true);
    expect(setupEquals(p, current({settings: [warnOn]}), kinds)).toBe(false);
    expect(setupEquals(p, current({settings: [dir]}), kinds)).toBe(false);
  });

  test('the trace cap never counts', () => {
    const p = preset({settings: []});
    expect(setupEquals(p, current({settings: [cap]}), kinds)).toBe(true);
  });

  test('the loose match must hold too', () => {
    const p = preset({traceOrderBy: 'x asc'});
    expect(setupEquals(p, current(), kinds)).toBe(false);
  });
});
