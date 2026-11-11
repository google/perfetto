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
import type {Trace} from '../../public/trace';
import {Button, ButtonVariant} from '../../widgets/button';
import {closeModal, showModal} from '../../widgets/modal';
import {Popup} from '../../widgets/popup';
import {Editor} from '../../widgets/editor';
import {TextInput} from '../../widgets/text_input';
import {Spinner} from '../../widgets/spinner';
import {STR} from '../../trace_processor/query_result';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import type {SourceSpec} from './types';

export interface TableItem {
  readonly name: string;
  readonly module?: string;
  readonly type?: string;
}

export interface SourcePickerAttrs {
  readonly trace: Trace;
  readonly currentSource?: SourceSpec;
  readonly onSelect: (spec: SourceSpec) => void;
}

/**
 * Loads available tables from trace processor and SqlModules.
 */
export async function loadAvailableTables(trace: Trace): Promise<TableItem[]> {
  const tableMap = new Map<string, TableItem>();

  // 1. Fetch tables and views from sqlite_master
  try {
    const res = await trace.engine.query(
      `SELECT name, type FROM sqlite_master
       WHERE type IN ('table', 'view')
         AND name NOT LIKE 'sqlite_%'
         AND name NOT LIKE '\\_%' ESCAPE '\\'
       ORDER BY name`,
    );
    const it = res.iter({name: STR, type: STR});
    for (; it.valid(); it.next()) {
      tableMap.set(it.name, {
        name: it.name,
        type: it.type,
      });
    }
  } catch {
    // Ignore query failure
  }

  // 2. Try fetching from perfetto_tables if available
  try {
    const res = await trace.engine.query(
      `SELECT name FROM perfetto_tables
       WHERE name NOT LIKE '\\_%' ESCAPE '\\'
       ORDER BY name`,
    );
    const it = res.iter({name: STR});
    for (; it.valid(); it.next()) {
      if (!tableMap.has(it.name)) {
        tableMap.set(it.name, {
          name: it.name,
          type: 'table',
        });
      }
    }
  } catch {
    // Ignore if table does not exist
  }

  // 3. Add stdlib tables from SqlModulesPlugin
  try {
    const sqlPlugin = trace.plugins.getPlugin(SqlModulesPlugin);
    const sqlModules = sqlPlugin?.getSqlModules();
    if (sqlModules !== undefined) {
      for (const table of sqlModules.listTables()) {
        tableMap.set(table.name, {
          name: table.name,
          module: table.includeKey,
          type: table.type,
        });
      }
    }
  } catch {
    // Ignore plugin error
  }

  return Array.from(tableMap.values()).sort((a, b) =>
    a.name.localeCompare(b.name),
  );
}

/**
 * Opens a full-screen modal allowing the user to enter and execute a custom SQL query.
 */
export function openCustomQueryModal(
  initialSql: string = '',
  onSelect: (spec: SourceSpec) => void,
): void {
  let queryText = initialSql;

  showModal({
    title: 'Plot Custom SQL Query',
    buttons: [
      {
        text: 'Plot',
        primary: true,
        action: () => {
          const sql = queryText.trim();
          if (sql.length > 0) {
            onSelect({kind: 'query', sql});
          }
        },
      },
      {
        text: 'Cancel',
      },
    ],
    content: () =>
      m(
        '.pf-scatter-query-modal',
        m(
          '.pf-scatter-query-modal__editor-container',
          m(Editor, {
            text: queryText,
            language: 'perfetto-sql',
            fillHeight: true,
            autofocus: true,
            onUpdate: (text) => {
              queryText = text;
            },
            onExecute: (text) => {
              const sql = text.trim();
              if (sql.length > 0) {
                closeModal();
                onSelect({kind: 'query', sql});
              }
            },
          }),
        ),
      ),
  });
}

/**
 * SourcePicker component providing fuzzy search over tables plus custom query entry.
 */
export class SourcePicker implements m.ClassComponent<SourcePickerAttrs> {
  private filterText = '';
  private tables: TableItem[] = [];
  private isLoading = true;

  oninit({attrs}: m.CVnode<SourcePickerAttrs>) {
    this.isLoading = true;
    loadAvailableTables(attrs.trace)
      .then((items) => {
        this.tables = items;
        this.isLoading = false;
        m.redraw();
      })
      .catch(() => {
        this.tables = [];
        this.isLoading = false;
        m.redraw();
      });
  }

  view({attrs}: m.CVnode<SourcePickerAttrs>) {
    const filter = this.filterText.trim().toLowerCase();
    const filteredTables =
      filter.length === 0
        ? this.tables
        : this.tables.filter((t) => {
            return (
              t.name.toLowerCase().includes(filter) ||
              (t.module !== undefined &&
                t.module.toLowerCase().includes(filter))
            );
          });

    return m(
      '.pf-scatter-source-picker',
      m(TextInput, {
        className: 'pf-scatter-source-picker__search',
        placeholder: 'Search tables...',
        leftIcon: 'search',
        autofocus: true,
        value: this.filterText,
        onInput: (val: string) => {
          this.filterText = val;
        },
      }),
      m(
        '.pf-scatter-source-picker__list',
        this.isLoading && m(Spinner),
        !this.isLoading &&
          filteredTables.length === 0 &&
          m('.pf-scatter-source-picker__empty', 'No matching tables'),
        !this.isLoading &&
          filteredTables.map((table) => {
            const badge = table.module ?? table.type;

            return m(
              'button.pf-scatter-source-picker__item',
              {
                key: table.name,
                // Close the enclosing PopupMenu once a table is picked.
                className: Popup.DISMISS_POPUP_GROUP_CLASS,
                onclick: () => {
                  attrs.onSelect({
                    kind: 'table',
                    table: table.name,
                    module: table.module,
                  });
                },
              },
              m('.pf-scatter-source-picker__item-name', table.name),
              badge !== undefined &&
                m('.pf-scatter-source-picker__item-badge', badge),
            );
          }),
      ),
      m(
        '.pf-scatter-source-picker__custom-btn',
        m(Button, {
          label: 'Custom SQL query...',
          icon: 'terminal',
          variant: ButtonVariant.Minimal,
          className: Popup.DISMISS_POPUP_GROUP_CLASS,
          onclick: () => {
            const initial =
              attrs.currentSource?.kind === 'query'
                ? attrs.currentSource.sql
                : '';
            openCustomQueryModal(initial, attrs.onSelect);
          },
        }),
      ),
    );
  }
}
