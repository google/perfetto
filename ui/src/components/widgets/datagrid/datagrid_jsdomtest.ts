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
import {afterEach, beforeEach, describe, expect, test} from 'vitest';
import {prettyDOM, queryAllByRole} from '@testing-library/dom';
import {DataGrid, type DataGridAttrs} from './datagrid';
import {InMemoryDataSource} from './in_memory_data_source';
import type {SchemaRegistry} from './datagrid_schema';
import type {Column, Filter} from './model';

// These tests render a DataGrid into a jsdom element and query the DOM to
// ensure correctness. It doesn't test styles or layout, only asserting the DOM
// tree.
//
// Each test renders a grid into `container` (jsdom, set globally in
// vitest.config.mjs) and queries it with @testing-library/dom. We render
// manually via m.render, so renderDataGrid() returns a `rerender` thunk to
// flush after interactions (m.redraw is a no-op here). Popup menus mount into a
// Portal on document.body, so menu items are queried there. Failures attach a
// prettyDOM() dump (dumpDom / dumpDom) as the assertion message.
//
// We check inline controls (the "Sort column" and chip "Remove filter" buttons)
// and popup menu items (opened from the column header). Per-cell menu items are
// not tested: the grid virtualizes body rows off the viewport height, which is
// zero under jsdom, so no cells render. The cell "Add filter" is gated on the
// same showFilterControls signal as the column-header one, so nothing goes
// untested.

const SCHEMA: SchemaRegistry = {
  root: {
    name: {title: 'Name', columnType: 'text'},
    value: {title: 'Value', columnType: 'quantitative'},
  },
};

const DATA = [
  {name: 'a', value: 1},
  {name: 'b', value: 2},
];

const COLUMNS: readonly Column[] = [
  {id: 'name', field: 'name'},
  {id: 'value', field: 'value'},
];

const FILTER: Filter = {field: 'value', op: '>', value: 0};

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

// Renders the grid into `container`. Returns a `rerender` thunk used to flush
// mithril after a click (we drive m.render manually, so the click handler's
// m.redraw() does not repaint on its own).
//
// Each rerender builds a FRESH vnode: m.render skips re-diffing if handed the
// same vnode object instance, so reusing one would make the re-render a no-op
// and the popup would never mount. A fresh vnode at the same position reuses
// the same DataGrid component instance, so model state (e.g. an open popup)
// still persists across rerenders.
function renderDataGrid(attrs: Partial<DataGridAttrs>) {
  const fullAttrs = {
    schema: SCHEMA,
    rootSchema: 'root',
    data: new InMemoryDataSource(DATA),
    ...attrs,
  };
  const rerender = () => m.render(container, m(DataGrid, fullAttrs));
  rerender();
  return rerender;
}

// Failure message: a pretty-printed dump of the rendered grid, so a mismatch
// shows the actual DOM rather than just a count.
function dumpDom(): string {
  const out = prettyDOM();
  return typeof out === 'string' ? out : '';
}

// Opens a context menu by clicking its trigger button (identified by aria
// label) and returns the labels of the menu items now mounted in the document.
// The menu renders through a Portal attached to document.body, so we query
// there rather than `container`. `rerender` is called after the click to flush
// the manual render so the popup mounts before we query it.
function getMenuItems(rerender: () => void, triggerLabel: string): string[] {
  const menuButton = queryAllByRole(container, 'button', {
    name: triggerLabel,
  })[0];
  expect(menuButton, dumpDom()).toBeTruthy();
  menuButton.dispatchEvent(new window.MouseEvent('click', {bubbles: true}));
  rerender();
  // Read the label element specifically: a menu item's textContent also
  // includes its icon's ligature text (e.g. "pivot_table_chart"), so we'd
  // otherwise get the icon name concatenated with the label.
  return Array.from(
    document.body.querySelectorAll<HTMLElement>('.pf-menu-item__label'),
  ).map((el) => el.textContent?.trim() ?? '');
}

// Opens the first column header's context menu (the per-column "⋮" button).
function getColumnMenuItems(rerender: () => void): string[] {
  return getMenuItems(rerender, 'Column menu');
}

// Whether the first header cell container is draggable (i.e. columns are
// reorderable). We read the `draggable` property, not the attribute: the grid
// sets `draggable={true|false}`, which reflects to a `draggable="false"`
// attribute when disabled, so hasAttribute() would be true either way.
function isFirstCellDraggable(): boolean {
  const cell = container.querySelector<HTMLElement>('.pf-grid__cell-container');
  return cell?.draggable === true;
}

describe('DataGrid column controls', () => {
  test('uncontrolled columns show sort buttons', () => {
    const rerender = renderDataGrid({});
    expect(
      queryAllByRole(container, 'button', {name: 'Sort column'}).length,
      dumpDom(),
    ).toBeGreaterThan(0);
    // Also expect that columns are reorderable (draggable)
    expect(isFirstCellDraggable(), dumpDom()).toBe(true);
    // Also assert that the add column and remove column menu items are present,
    // which also only render when columns are mutable.
    const items = getColumnMenuItems(rerender);
    expect(items, dumpDom()).toContain('Add column');
    expect(items, dumpDom()).toContain('Remove column');
  });

  test('controlled columns with onColumnsChanged show sort buttons', () => {
    const rerender = renderDataGrid({
      columns: COLUMNS,
      onColumnsChanged: () => {},
    });
    expect(
      queryAllByRole(container, 'button', {name: 'Sort column'}).length,
      dumpDom(),
    ).toBeGreaterThan(0);
    // Also expect that columns are reorderable (draggable)
    expect(isFirstCellDraggable(), dumpDom()).toBe(true);
    // Also assert that the add column and remove column menu items are present,
    // which also only render when columns are mutable.
    const items = getColumnMenuItems(rerender);
    expect(items, dumpDom()).toContain('Add column');
    expect(items, dumpDom()).toContain('Remove column');
  });

  test('controlled columns without callback hide sort buttons', () => {
    renderDataGrid({
      columns: COLUMNS,
    });
    expect(
      queryAllByRole(container, 'button', {name: 'Sort column'}),
      dumpDom(),
    ).toHaveLength(0);
  });

  test('disableColumnControls hides sort buttons even when uncontrolled', () => {
    const rerender = renderDataGrid({
      disableColumnControls: true,
    });
    expect(
      queryAllByRole(container, 'button', {name: 'Sort column'}),
      dumpDom(),
    ).toHaveLength(0);
    expect(isFirstCellDraggable(), dumpDom()).toBe(false);
    // Also assert that the add column and remove column menu items are present,
    // which also only render when columns are mutable.
    const items = getColumnMenuItems(rerender);
    expect(items, dumpDom()).not.toContain('Add column');
    expect(items, dumpDom()).not.toContain('Remove column');
  });

  test('disableColumnControls overrides a wired callback', () => {
    const rerender = renderDataGrid({
      columns: COLUMNS,
      onColumnsChanged: () => {},
      disableColumnControls: true,
    });
    expect(
      queryAllByRole(container, 'button', {name: 'Sort column'}),
      dumpDom(),
    ).toHaveLength(0);
    expect(isFirstCellDraggable(), dumpDom()).toBe(false);
    // Also assert that the add column and remove column menu items are present,
    // which also only render when columns are mutable.
    const items = getColumnMenuItems(rerender);
    expect(items, dumpDom()).not.toContain('Add column');
    expect(items, dumpDom()).not.toContain('Remove column');
  });
});

describe('DataGrid filter controls', () => {
  test('uncontrolled filters show the chip remove button', () => {
    const rerender = renderDataGrid({
      initialFilters: [FILTER],
    });
    expect(
      queryAllByRole(container, 'button', {name: 'Remove filter'}).length,
      dumpDom(),
    ).toBeGreaterThan(0);
    // The column header menu also offers "Add filter" when filters are mutable.
    expect(getColumnMenuItems(rerender), dumpDom()).toContain('Add filter');
  });

  test('controlled filters with onFiltersChanged show the remove button', () => {
    const rerender = renderDataGrid({
      filters: [FILTER],
      onFiltersChanged: () => {},
    });
    expect(
      queryAllByRole(container, 'button', {name: 'Remove filter'}).length,
      dumpDom(),
    ).toBeGreaterThan(0);
    expect(getColumnMenuItems(rerender), dumpDom()).toContain('Add filter');
  });

  test('controlled filters without callback hide the remove button', () => {
    const rerender = renderDataGrid({
      filters: [FILTER],
    });
    expect(
      queryAllByRole(container, 'button', {name: 'Remove filter'}),
      dumpDom(),
    ).toHaveLength(0);
    // The "Add filter" menu item is gated on the same showFilterControls signal.
    expect(getColumnMenuItems(rerender), dumpDom()).not.toContain('Add filter');
  });

  test('disableFilterControls hides the remove button', () => {
    const rerender = renderDataGrid({
      filters: [FILTER],
      onFiltersChanged: () => {},
      disableFilterControls: true,
    });
    expect(
      queryAllByRole(container, 'button', {name: 'Remove filter'}),
      dumpDom(),
    ).toHaveLength(0);
    expect(getColumnMenuItems(rerender), dumpDom()).not.toContain('Add filter');
  });
});

describe('DataGrid column menu', () => {
  test('uncontrolled grid offers "Group by this column"', () => {
    const rerender = renderDataGrid({});
    const items = getColumnMenuItems(rerender);
    expect(items, dumpDom()).toContain('Group by this column');
  });

  test('disablePivotControls hides "Group by this column"', () => {
    const rerender = renderDataGrid({disablePivotControls: true});
    const items = getColumnMenuItems(rerender);
    expect(items, dumpDom()).not.toContain('Group by this column');
  });

  test('controlled pivot with callback show the pivot controls', () => {
    const rerender = renderDataGrid({
      pivot: {groupBy: [], aggregates: []},
      onPivotChanged: () => {},
    });
    const items = getColumnMenuItems(rerender);
    expect(items, dumpDom()).toContain('Group by this column');
  });

  test('controlled pivot with no callback hides the pivot controls', () => {
    const rerender = renderDataGrid({
      pivot: {groupBy: [], aggregates: []},
    });
    const items = getColumnMenuItems(rerender);
    expect(items, dumpDom()).not.toContain('Group by this column');
  });
});
