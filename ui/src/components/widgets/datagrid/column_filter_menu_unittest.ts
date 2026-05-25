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

import m from 'mithril';
import type {SqlValue} from '../../../trace_processor/query_result';
import {
  DistinctValuesSubmenu,
  EditFilterMenu,
  type EditFilterMenuAttrs,
  TextFilterSubmenu,
} from './column_filter_menu';
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

// Test-local union of attrs across both submenu types — saves
// exporting their private interfaces. onApply: unknown so both Set
// and primitive callers pass without `any`.
interface InspectableSubmenuAttrs {
  readonly initialSelectedValues?: ReadonlyArray<SqlValue>;
  readonly excludeNull?: boolean;
  readonly inputType?: 'text' | 'number';
  readonly initialValue?: string;
  readonly submitLabel?: string;
  readonly onApply: (value: unknown) => void;
}

// Helper: render EditFilterMenu's view() and return the resulting
// vnode (or null). Tests inspect tag + attrs to assert dispatch.
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

// Narrow m.Children to a single Vnode for tag/attrs assertions; cast
// targets the test-local union since the concrete type varies.
function asVnode(c: m.Children): m.Vnode<InspectableSubmenuAttrs> {
  return c as unknown as m.Vnode<InspectableSubmenuAttrs>;
}

describe('EditFilterMenu — dispatch by op', () => {
  test.each(['in', 'not in'] as const)(
    '%s → DistinctValuesSubmenu with array seed',
    (op) => {
      const v = asVnode(editView({op, value: ['x', 'y']}, 'text'));
      expect(v.tag).toBe(DistinctValuesSubmenu);
      expect(v.attrs.initialSelectedValues).toEqual(['x', 'y']);
      expect(v.attrs.excludeNull).toBe(true);
    },
  );

  test.each(['text', 'identifier', undefined] as const)(
    '= on %s column → DistinctValuesSubmenu with single-element seed',
    (columnType) => {
      const v = asVnode(editView({op: '=', value: 'foo'}, columnType));
      expect(v.tag).toBe(DistinctValuesSubmenu);
      expect(v.attrs.initialSelectedValues).toEqual(['foo']);
    },
  );

  test('= on quantitative → TextFilterSubmenu(number)', () => {
    const v = asVnode(editView({op: '=', value: 100}, 'quantitative'));
    expect(v.tag).toBe(TextFilterSubmenu);
    expect(v.attrs.inputType).toBe('number');
    expect(v.attrs.initialValue).toBe('100');
    expect(v.attrs.submitLabel).toBe('Save');
  });

  test('!= on text → DistinctValuesSubmenu', () => {
    const v = asVnode(editView({op: '!=', value: 'foo'}, 'text'));
    expect(v.tag).toBe(DistinctValuesSubmenu);
  });

  test('!= on quantitative → TextFilterSubmenu(number)', () => {
    const v = asVnode(editView({op: '!=', value: 1}, 'quantitative'));
    expect(v.tag).toBe(TextFilterSubmenu);
    expect(v.attrs.inputType).toBe('number');
  });

  test.each(['<', '<=', '>', '>='] as const)(
    '%s → TextFilterSubmenu(number)',
    (op) => {
      const v = asVnode(editView({op, value: 100}, 'quantitative'));
      expect(v.tag).toBe(TextFilterSubmenu);
      expect(v.attrs.inputType).toBe('number');
      expect(v.attrs.initialValue).toBe('100');
      expect(v.attrs.submitLabel).toBe('Save');
    },
  );

  test.each(['glob', 'not glob'] as const)(
    '%s → TextFilterSubmenu(text)',
    (op) => {
      const v = asVnode(editView({op, value: '*hello*'}, 'text'));
      expect(v.tag).toBe(TextFilterSubmenu);
      expect(v.attrs.inputType).toBe('text');
      expect(v.attrs.initialValue).toBe('*hello*');
    },
  );

  test('is null → null (no editor)', () => {
    expect(editView({op: 'is null'}, 'text')).toBeNull();
  });

  test('is not null → null (no editor)', () => {
    expect(editView({op: 'is not null'}, 'quantitative')).toBeNull();
  });

  test('malformed {op: "=", value: null} → null (refuse to edit)', () => {
    // Legal per the type but never produced by add-mode (multi-select
    // emits arrays); refuse rather than render an empty editor.
    expect(editView({op: '=', value: null}, 'quantitative')).toBeNull();
    expect(editView({op: '!=', value: null}, 'text')).toBeNull();
    expect(editView({op: '>', value: null}, 'quantitative')).toBeNull();
    expect(editView({op: 'glob', value: null}, 'text')).toBeNull();
  });
});

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

describe('DistinctValuesSubmenu — initial state', () => {
  afterEach(() => {
    document.body.innerHTML = '';
  });

  // Locate Apply via its label element (the icon glyph pollutes the
  // button's textContent).
  function findApplyButton(root: HTMLElement): HTMLButtonElement | undefined {
    const labels = Array.from(
      root.querySelectorAll('.pf-menu-item__label'),
    ) as HTMLElement[];
    const applyLabel = labels.find(
      (el) => (el.textContent ?? '').trim() === 'Apply',
    );
    return (
      (applyLabel?.closest(
        'button.pf-menu-item',
      ) as HTMLButtonElement | null) ?? undefined
    );
  }

  test('Apply button is enabled iff initialSelectedValues is non-empty', () => {
    const renderWithSeed = (
      initialSelectedValues?: ReadonlyArray<SqlValue>,
    ): HTMLButtonElement => {
      const root = document.createElement('div');
      document.body.appendChild(root);
      m.render(
        root,
        m(DistinctValuesSubmenu, {
          datasource: makeStubDataSource(['a', 'b', 'c']),
          field: 'col',
          valueFormatter: (v) => String(v),
          initialSelectedValues,
          onApply: vi.fn(),
        }),
      );
      const applyButton = findApplyButton(root);
      expect(applyButton).toBeDefined();
      return applyButton!;
    };
    expect(renderWithSeed(['a']).disabled).toBe(false);
    expect(renderWithSeed(undefined).disabled).toBe(true);
  });

  // Return the visible distinct-value labels in DOM order (excludes
  // Apply / Clear footer items, which live in a sibling __footer).
  function listLabels(root: HTMLElement): string[] {
    const items = Array.from(
      root.querySelectorAll('.pf-distinct-values-menu__list .pf-menu-item'),
    ) as HTMLElement[];
    return items.map((el) => {
      const label = el.querySelector('.pf-menu-item__label');
      return (label?.textContent ?? '').trim();
    });
  }

  test('pinned: initial selection renders at the top in original order', () => {
    const root = document.createElement('div');
    document.body.appendChild(root);
    m.render(
      root,
      m(DistinctValuesSubmenu, {
        datasource: makeStubDataSource(['alpha', 'beta', 'gamma', 'delta']),
        field: 'col',
        valueFormatter: (v) => String(v),
        // gamma comes after alpha+beta in the source list but pinning
        // bumps both pinned items to the top, preserving the source
        // order WITHIN the pinned group.
        initialSelectedValues: ['gamma', 'alpha'],
        onApply: vi.fn(),
      }),
    );
    // Pinned items first (in source order: alpha before gamma),
    // then the unpinned rest (in source order: beta, delta).
    expect(listLabels(root)).toEqual(['alpha', 'gamma', 'beta', 'delta']);
  });
});
