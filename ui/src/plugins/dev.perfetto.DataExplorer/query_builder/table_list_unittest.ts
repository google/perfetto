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

import {searchAndRankTables} from './table_list';
import type {SqlColumn, SqlTable} from '../../dev.perfetto.SqlModules/sql_modules';

function mkTable(
  name: string,
  opts: {
    importance?: 'core' | 'high' | 'mid' | 'low';
    description?: string;
    columns?: ReadonlyArray<{name: string; description?: string}>;
  } = {},
): {table: SqlTable; moduleName: string} {
  const columns: SqlColumn[] = (opts.columns ?? []).map((c) => ({
    name: c.name,
    description: c.description,
  }));
  const table = {
    name,
    description: opts.description ?? '',
    type: 'TABLE',
    importance: opts.importance,
    columns,
    getTableColumns: () => [],
  } as unknown as SqlTable;
  return {table, moduleName: `module.${name}`};
}

describe('searchAndRankTables', () => {
  it('ranks strong fuzzy matches above weak high-importance ones', () => {
    // Regression test for #6559: searching `androidx_` returned only the fuzzy
    // stdlib `android_*` matches (which are high importance) while the strong
    // extension `androidx_*` matches were buried below them. Importance must
    // not override a clearly better fuzzy match.
    const tables = [
      mkTable('android_frames', {importance: 'high'}),
      mkTable('android_binder_txns', {importance: 'high'}),
      mkTable('android_input_events', {importance: 'core'}),
      mkTable('androidx_art_metrics'),
      mkTable('androidx_frame_timing'),
    ];

    const ranked = searchAndRankTables(tables, 'androidx_').map(
      (r) => r.item.table.name,
    );

    expect(ranked.slice(0, 2)).toEqual([
      'androidx_art_metrics',
      'androidx_frame_timing',
    ]);
  });

  it('orders matches of comparable relevance by importance', () => {
    const tables = [
      mkTable('thread_state', {importance: 'mid'}),
      mkTable('thread', {importance: 'core'}),
      mkTable('thread_track', {importance: 'high'}),
    ];

    const ranked = searchAndRankTables(tables, 'thread').map(
      (r) => r.item.table.name,
    );

    expect(ranked).toEqual(['thread', 'thread_track', 'thread_state']);
  });

  it('ranks name matches above column and description matches', () => {
    const tables = [
      mkTable('unrelated', {
        columns: [{name: 'slice_id'}],
      }),
      mkTable('has_desc', {description: 'mentions slice somewhere'}),
      mkTable('slice_table'),
    ];

    const ranked = searchAndRankTables(tables, 'slice').map(
      (r) => r.item.table.name,
    );

    expect(ranked[0]).toEqual('slice_table');
    expect(ranked).toContain('unrelated');
    expect(ranked).toContain('has_desc');
  });

  it('preserves importance ordering when browsing with an empty query', () => {
    const tables = [
      mkTable('b_table', {importance: 'low'}),
      mkTable('a_table', {importance: 'core'}),
    ];

    const ranked = searchAndRankTables(tables, '').map(
      (r) => r.item.table.name,
    );

    expect(ranked).toEqual(['a_table', 'b_table']);
  });
});
