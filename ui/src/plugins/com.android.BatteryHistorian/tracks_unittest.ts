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
  availableGroups,
  CHART_COUNTERS,
  GROUPS,
  rowQuery,
  wantedTracks,
} from './tracks';

describe('BatteryHistorian tracks', () => {
  test('row names are unique within a group', () => {
    for (const group of GROUPS) {
      const names = group.rows.map((r) => r.name);
      expect(new Set(names).size).toBe(names.length);
    }
  });

  test('every track is shown at most once', () => {
    const tracks = GROUPS.flatMap((g) => g.rows.map((r) => r.track));
    expect(new Set(tracks).size).toBe(tracks.length);
  });

  test('wanted tracks cover the chart and every row', () => {
    const wanted = new Set(wantedTracks());
    for (const c of CHART_COUNTERS) expect(wanted.has(c.track)).toBe(true);
    for (const g of GROUPS) {
      for (const r of g.rows) expect(wanted.has(r.track)).toBe(true);
    }
  });

  test('keeps only the rows and groups the trace has tracks for', () => {
    const groups = availableGroups(
      new Set(['battery_stats.screen', 'battery_stats.job', 'PlugType']),
    );
    expect(groups.map((g) => [g.name, g.rows.map((r) => r.name)])).toEqual([
      ['Power', ['Plugged']],
      ['System', ['Screen on', 'Jobs']],
    ]);
  });

  test('keeps nothing without battery stats', () => {
    expect(availableGroups(new Set())).toEqual([]);
  });

  test('flag rows show only the on state, named after the row', () => {
    const sql = rowQuery({
      kind: 'flag',
      name: 'Screen on',
      track: 'battery_stats.screen',
    });
    expect(sql).toContain(`'Screen on' AS name`);
    expect(sql).toContain(`track_name = 'battery_stats.screen'`);
    expect(sql).toContain('value != 0');
  });

  test('enum rows hide the given values', () => {
    const sql = rowQuery({
      kind: 'enum',
      name: 'Doze',
      track: 'battery_stats.device_idle',
      hide: ['off'],
    });
    expect(sql).toContain('value_name AS name');
    expect(sql).toContain(`value_name NOT IN ('off')`);
  });

  test('event rows pair start and finish events and keep instants', () => {
    const sql = rowQuery({
      kind: 'event',
      name: 'Jobs',
      track: 'battery_stats.job',
    });
    expect(sql).toContain('android_battery_stats_event_slices');
    expect(sql).toContain(`NOT IN ('+', '-')`);
  });

  test('quotes track names', () => {
    const sql = rowQuery({kind: 'flag', name: `it's`, track: `a'b`});
    expect(sql).toContain(`'it''s' AS name`);
    expect(sql).toContain(`track_name = 'a''b'`);
  });

  test('maps plug types to names and hides unplugged', () => {
    const plugged = GROUPS[0]?.rows[0];
    expect(plugged?.name).toBe('Plugged');
    const sql = rowQuery(plugged!);
    expect(sql).toContain(`WHEN 1 THEN 'AC'`);
    expect(sql).toContain('WHERE value NOT IN (0)');
  });
});
