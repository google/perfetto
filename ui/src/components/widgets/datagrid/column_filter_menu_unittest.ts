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

import type m from 'mithril';
import type {SqlValue} from '../../../trace_processor/query_result';
import {EditFilterMenu, type EditFilterMenuAttrs} from './column_filter_menu';
import type {DataSource} from './data_source';
import type {ColumnType} from './datagrid_schema';
import type {FilterOpAndValue} from './model';

// DataSource stub: only useDistinctValues is wired; everything else
// throws so tests fail loudly on unexpected use.
function makeStubDataSource(distinctData?: readonly SqlValue[]): DataSource {
  return {
    useDistinctValues: () => ({
      data: distinctData,
      isPending: distinctData === undefined,
      isFresh: true,
    }),
  } as unknown as DataSource;
}

// Test-local view of the submenu attrs we care about — onApply is the
// hook op-promotion goes through.
interface InspectableSubmenuAttrs {
  readonly onApply: (value: unknown) => void;
}

function editView(
  initialFilter: FilterOpAndValue,
  columnType: ColumnType | undefined,
  overrides: Partial<EditFilterMenuAttrs> = {},
): m.Children {
  const inst = new EditFilterMenu();
  return inst.view({
    attrs: {
      datasource: makeStubDataSource(['a', 'b', 'c']),
      field: 'col',
      columnType,
      valueFormatter: (v: SqlValue) => String(v),
      initialFilter,
      onFilterReplace: vi.fn(),
      ...overrides,
    },
  } as unknown as m.Vnode<EditFilterMenuAttrs>);
}

function asVnode(c: m.Children): m.Vnode<InspectableSubmenuAttrs> {
  return c as unknown as m.Vnode<InspectableSubmenuAttrs>;
}

// On text columns, `=` / `!=` editing promotes to `in` / `not in` on
// save (the editor is a multi-select, so the apply payload is a Set).
// On quantitative columns and on already-set ops, no promotion.
describe('EditFilterMenu — op promotion on save', () => {
  test.each([
    ['=', 'in'],
    ['!=', 'not in'],
  ] as const)(
    '%s on text + apply emits {op: "%s", value: [...]}',
    (op, promoted) => {
      const onFilterReplace = vi.fn();
      const v = asVnode(
        editView({op, value: 'foo'}, 'text', {onFilterReplace}),
      );
      v.attrs.onApply(new Set(['foo', 'bar']));
      expect(onFilterReplace).toHaveBeenCalledWith({
        op: promoted,
        value: ['foo', 'bar'],
      });
    },
  );

  test('= on quantitative + apply preserves op', () => {
    const onFilterReplace = vi.fn();
    const v = asVnode(
      editView({op: '=', value: 100}, 'quantitative', {onFilterReplace}),
    );
    v.attrs.onApply(200);
    expect(onFilterReplace).toHaveBeenCalledWith({op: '=', value: 200});
  });

  test('in + apply preserves op', () => {
    const onFilterReplace = vi.fn();
    const v = asVnode(
      editView({op: 'in', value: ['a']}, 'text', {onFilterReplace}),
    );
    v.attrs.onApply(new Set(['a', 'b', 'c']));
    expect(onFilterReplace).toHaveBeenCalledWith({
      op: 'in',
      value: ['a', 'b', 'c'],
    });
  });

  test('> + apply preserves op', () => {
    const onFilterReplace = vi.fn();
    const v = asVnode(
      editView({op: '>', value: 100}, 'quantitative', {onFilterReplace}),
    );
    v.attrs.onApply(500);
    expect(onFilterReplace).toHaveBeenCalledWith({op: '>', value: 500});
  });

  test('glob + apply preserves op', () => {
    const onFilterReplace = vi.fn();
    const v = asVnode(
      editView({op: 'glob', value: '*x*'}, 'text', {onFilterReplace}),
    );
    v.attrs.onApply('*y*');
    expect(onFilterReplace).toHaveBeenCalledWith({op: 'glob', value: '*y*'});
  });
});
