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
import {Icon} from '../../widgets/icon';
import {TextInput} from '../../widgets/text_input';
import {SqlModules, SqlTable} from '../dev.perfetto.SqlModules/sql_modules';

export interface SimpleTableListAttrs {
  readonly sqlModules: SqlModules;
  readonly onTableClick: (tableName: string) => void;
}

export class SimpleTableList implements m.ClassComponent<SimpleTableListAttrs> {
  private searchQuery = '';
  private expandedTables = new Set<string>();

  view({attrs}: m.CVnode<SimpleTableListAttrs>): m.Children {
    const tables = attrs.sqlModules.listTables();

    // Filter tables by search query
    const filteredTables = this.searchQuery.trim()
      ? tables.filter((t) =>
          t.name.toLowerCase().includes(this.searchQuery.toLowerCase()),
        )
      : tables;

    return m(
      '.pf-simple-table-list',
      m(TextInput, {
        className: 'pf-simple-table-list__search',
        placeholder: 'Search tables...',
        value: this.searchQuery,
        leftIcon: 'search',
        onInput: (value) => {
          this.searchQuery = value;
        },
      }),
      m(
        '.pf-simple-table-list__items',
        filteredTables.map((table) =>
          this.renderTableItem(table, attrs.onTableClick),
        ),
      ),
    );
  }

  private renderTableItem(
    table: SqlTable,
    onTableClick: (tableName: string) => void,
  ): m.Children {
    const isExpanded = this.expandedTables.has(table.name);

    return m(
      '.pf-simple-table-list__item',
      {className: isExpanded ? 'pf-expanded' : ''},
      // Header row with expand toggle and table name
      m(
        '.pf-simple-table-list__item-header',
        m(
          '.pf-simple-table-list__item-toggle',
          {
            onclick: () => {
              if (isExpanded) {
                this.expandedTables.delete(table.name);
              } else {
                this.expandedTables.add(table.name);
              }
            },
          },
          m(Icon, {icon: isExpanded ? 'expand_more' : 'chevron_right'}),
        ),
        m(
          '.pf-simple-table-list__item-name',
          {onclick: () => onTableClick(table.name)},
          table.name,
        ),
      ),
      // Expandable details
      isExpanded &&
        m(
          '.pf-simple-table-list__item-details',
          // Module
          table.includeKey &&
            m(
              '.pf-simple-table-list__detail-row',
              m('span.pf-simple-table-list__detail-label', 'Module'),
              m('code.pf-simple-table-list__detail-value', table.includeKey),
            ),
          // Columns
          table.columns.length > 0 &&
            m(
              '.pf-simple-table-list__columns',
              m('span.pf-simple-table-list__detail-label', 'Columns'),
              m(
                '.pf-simple-table-list__column-list',
                table.columns.map((col) =>
                  m(
                    '.pf-simple-table-list__column',
                    m('code.pf-simple-table-list__column-name', col.name),
                    m(
                      'span.pf-simple-table-list__column-type',
                      col.type ?? 'unknown',
                    ),
                  ),
                ),
              ),
            ),
        ),
    );
  }
}
