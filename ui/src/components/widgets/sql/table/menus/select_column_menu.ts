// Copyright (C) 2025 The Android Open Source Project
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
import {TableColumn, ListColumnsContext, tableColumnId} from '../table_column';
import {MenuDivider, MenuItem} from '../../../../../widgets/menu';
import {raf} from '../../../../../core/raf_scheduler';
import {uuidv4} from '../../../../../base/uuid';
import {hasModKey, modKey} from '../../../../../base/hotkeys';
import {TextInput} from '../../../../../widgets/text_input';
import {Spinner} from '../../../../../widgets/spinner';
import {Filters} from '../filters';
import {Trace} from '../../../../../public/trace';
import {SqlColumn} from '../sql_column';

export type SelectColumnMenuAttrs = {
  columns:
    | {key: string; column: TableColumn}[]
    | (() => Promise<{key: string; column: TableColumn}[]>);
  primaryColumn?: {key: string; column: TableColumn};
  filterable?: 'on' | 'off';
  filters: Filters;
  trace: Trace;
  getSqlQuery(data: {[key: string]: SqlColumn}): string;
  existingColumnIds?: Set<string>;
  // Possible actions when a column is selected.
  // - Run a callback.
  onColumnSelected?: (column: TableColumn) => void;
  // - Show a nested menu.
  columnMenu?: (column: TableColumn) => {
    rightIcon?: string;
    children: m.Children;
  };
};

type SelectColumnMenuImplAttrs = {
  columns: {key: string; column: TableColumn}[];
  filters: Filters;
  trace: Trace;
  getSqlQuery(data: {[key: string]: SqlColumn}): string;
  existingColumnIds?: Set<string>;
  firstButtonUuid: string;
  onColumnSelected?: (column: TableColumn) => void;
  columnMenu?: (column: TableColumn) => {
    rightIcon?: string;
    children: m.Children;
  };
};

function onColumnSelectedClickHandler(
  column: TableColumn,
  onColumnSelected?: (column: TableColumn) => void,
): undefined | ((event: PointerEvent) => void) {
  if (onColumnSelected === undefined) return undefined;
  return (event: PointerEvent) => {
    onColumnSelected(column);
    // For Control-Click, we don't want to close the menu to allow the user
    // to select multiple items in one go.
    if (hasModKey(event)) {
      event.stopPropagation();
    }
    // Otherwise this popup will be closed.
  };
}

// Core implementation of the selectable column list.
class SelectColumnMenuImpl
  implements m.ClassComponent<SelectColumnMenuImplAttrs>
{
  // When the menu elements are updated (e.g. when filtering), the popup
  // can flicker a lot. To prevent that, we fix the size of the popup
  // after the first layout.
  private size?: {width: number; height: number};

  oncreate(vnode: m.VnodeDOM<SelectColumnMenuImplAttrs, this>) {
    this.size = {
      width: vnode.dom.clientWidth,
      height: vnode.dom.clientHeight,
    };
  }

  view({attrs}: m.CVnode<SelectColumnMenuImplAttrs>) {
    const context: ListColumnsContext = {
      filters: attrs.filters,
      trace: attrs.trace,
      getSqlQuery: attrs.getSqlQuery,
    };

    return m(
      '.pf-sql-table__select-column-menu',
      {
        style: {
          minWidth: this.size && `${this.size.width}px`,
          minHeight: this.size && `${this.size.height}px`,
        },
      },
      attrs.columns.map(({key, column}, index) => {
        const derivedColumns = column.listDerivedColumns?.(context);
        const columnMenu =
          derivedColumns === undefined ? attrs.columnMenu?.(column) : undefined;
        return m(
          MenuItem,
          {
            id: index === 0 ? attrs.firstButtonUuid : undefined,
            label: key,
            rightIcon: columnMenu?.rightIcon,
            onclick:
              derivedColumns === undefined
                ? onColumnSelectedClickHandler(column, attrs.onColumnSelected)
                : undefined,
          },
          derivedColumns !== undefined &&
            m(SelectColumnMenu, {
              primaryColumn: {key, column},
              existingColumnIds: attrs.existingColumnIds,
              onColumnSelected: attrs.onColumnSelected,
              columnMenu: attrs.columnMenu,
              filters: attrs.filters,
              trace: attrs.trace,
              getSqlQuery: attrs.getSqlQuery,
              columns: async () => {
                const cols = await derivedColumns();
                return [...cols.entries()].map(([key, column]) => ({
                  key,
                  column,
                }));
              },
            }),
          columnMenu?.children,
        );
      }),
    );
  }
}

export class SelectColumnMenu
  implements m.ClassComponent<SelectColumnMenuAttrs>
{
  private searchText = '';
  columns?: {key: string; column: TableColumn}[];

  constructor(vnode: m.CVnode<SelectColumnMenuAttrs>) {
    if (Array.isArray(vnode.attrs.columns)) {
      this.columns = vnode.attrs.columns;
    } else {
      vnode.attrs.columns().then((columns) => {
        this.columns = columns;
        raf.scheduleFullRedraw();
      });
    }
  }

  view(vnode: m.CVnode<SelectColumnMenuAttrs>) {
    const columns = this.columns || [];
    const {attrs} = vnode;

    const context: ListColumnsContext = {
      filters: attrs.filters,
      trace: attrs.trace,
      getSqlQuery: attrs.getSqlQuery,
    };

    // Candidates are the columns which have not been selected yet.
    const candidates = [...columns].filter(
      ({column}) =>
        !attrs.existingColumnIds?.has(tableColumnId(column)) ||
        column.listDerivedColumns?.(context) !== undefined,
    );

    const filterable =
      attrs.filterable === 'on' ||
      (attrs.filterable === undefined && candidates.length > 10);

    // Filter the candidates based on the search text.
    const filtered = candidates.filter(({key}) => {
      return key.toLowerCase().includes(this.searchText.toLowerCase());
    });

    const primaryColumn = attrs.primaryColumn;
    const primaryColumnMenu =
      primaryColumn === undefined
        ? undefined
        : attrs.columnMenu?.(primaryColumn?.column);
    const firstButtonUuid = uuidv4();

    return [
      primaryColumn &&
        m(
          MenuItem,
          {
            label: primaryColumn.key,
            disabled: attrs.existingColumnIds?.has(
              tableColumnId(primaryColumn.column),
            ),
            onclick: onColumnSelectedClickHandler(
              primaryColumn.column,
              attrs.onColumnSelected,
            ),
            rightIcon: primaryColumnMenu?.rightIcon,
          },
          primaryColumnMenu?.children,
        ),
      primaryColumn && m(MenuDivider),
      filterable &&
        m(TextInput, {
          autofocus: true,
          oninput: (event: Event) => {
            const eventTarget = event.target as HTMLTextAreaElement;
            this.searchText = eventTarget.value;
          },
          onkeydown: (event: KeyboardEvent) => {
            if (filtered.length === 0) return;
            if (event.key === 'Enter') {
              // If there is only one item or Mod-Enter was pressed, select the first element.
              if (filtered.length === 1 || hasModKey(event)) {
                const params = {bubbles: true};
                if (hasModKey(event)) {
                  Object.assign(params, modKey());
                }
                const pointerEvent = new PointerEvent('click', params);
                (
                  document.getElementById(firstButtonUuid) as HTMLElement | null
                )?.dispatchEvent(pointerEvent);
              }
            }
          },
          value: this.searchText,
          placeholder: 'Filter...',
          className: 'pf-sql-table__column-filter',
        }),
      filterable && m(MenuDivider),
      this.columns === undefined && m(Spinner),
      this.columns !== undefined &&
        m(SelectColumnMenuImpl, {
          columns: filtered,
          filters: attrs.filters,
          trace: attrs.trace,
          getSqlQuery: attrs.getSqlQuery,
          existingColumnIds: attrs.existingColumnIds,
          onColumnSelected: attrs.onColumnSelected,
          columnMenu: attrs.columnMenu,
          firstButtonUuid,
        }),
    ];
  }
}
