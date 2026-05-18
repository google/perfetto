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
import {FuzzyFinder, type FuzzySegment} from '../../base/fuzzy';
import {Accordion, AccordionSection} from '../../widgets/accordion';
import {Button} from '../../widgets/button';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {Icon} from '../../widgets/icon';
import {TextInput} from '../../widgets/text_input';
import type {SqlModules, SqlTable} from './sql_modules';
import {
  perfettoSqlTypeIcon,
  perfettoSqlTypeToString,
} from '../../trace_processor/perfetto_sql_type';
import {EmptyState} from '../../widgets/empty_state';

interface FilteredTable {
  table: SqlTable;
  segments: FuzzySegment[];
}

// Single source so run-query, body, title, and Copy text stay in sync.
function includeStatement(includeKey: string): string {
  return `INCLUDE PERFETTO MODULE ${includeKey};`;
}

function renderHighlightedName(segments: FuzzySegment[]): m.Children {
  return segments.map(({matching, value}) =>
    matching ? m('span.pf-simple-table-list__highlight', value) : value,
  );
}

export interface TableListAttrs {
  readonly sqlModules: SqlModules;
  // Called when user wants to query a table in a new tab
  onQueryTable?(tableName: string, query: string): void;
}

export class TableList implements m.ClassComponent<TableListAttrs> {
  private searchQuery = '';

  view({attrs}: m.CVnode<TableListAttrs>): m.Children {
    const tables = attrs.sqlModules.listTables();

    // Filter tables using fuzzy search (results ordered by relevance)
    const searchTerm = this.searchQuery.trim();
    let filteredTables: FilteredTable[];
    if (searchTerm === '') {
      filteredTables = tables.map((table) => ({
        table,
        segments: [{matching: false, value: table.name}],
      }));
    } else {
      const finder = new FuzzyFinder(tables, (t) => t.name);
      filteredTables = finder.find(searchTerm).map((result) => ({
        table: result.item,
        segments: result.segments,
      }));
    }

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
      filteredTables.length > 0
        ? m(
            '.pf-simple-table-list__items',
            m(
              Accordion,
              // multi:true so users can expand several tables for schema compare.
              {multi: true},
              filteredTables.map(({table, segments}) =>
                m(
                  AccordionSection,
                  {
                    key: table.name,
                    summary: m(
                      'code.pf-simple-table-list__item-name',
                      // Ellipsis-truncated; tooltip reveals the full name.
                      {title: table.name},
                      renderHighlightedName(segments),
                    ),
                  },
                  this.renderTableContent(table, attrs.onQueryTable),
                ),
              ),
            ),
          )
        : m(EmptyState, {
            title: 'No matching tables found',
          }),
    );
  }

  private renderIncludeRow(includeKey: string): m.Children {
    const include = includeStatement(includeKey);
    return m(
      '.pf-simple-table-list__detail-row',
      m('span.pf-simple-table-list__detail-label', 'Include'),
      m('code.pf-simple-table-list__detail-value', {title: include}, include),
      m(CopyToClipboardButton, {
        className: 'pf-show-on-hover',
        textToCopy: include,
        tooltip: 'Copy include string to clipboard',
      }),
    );
  }

  private generateQuery(table: SqlTable): string {
    const lines: string[] = [];

    // Add INCLUDE statement if needed
    if (table.includeKey) {
      lines.push(includeStatement(table.includeKey));
      lines.push('');
    }

    // Build SELECT with all columns
    const columns =
      table.columns.length > 0
        ? table.columns.map((c) => c.name).join(',\n  ')
        : '*';

    lines.push('SELECT');
    lines.push(`  ${columns}`);
    lines.push(`FROM ${table.name}`);
    lines.push('LIMIT 1000');

    return lines.join('\n');
  }

  private renderTableContent(
    table: SqlTable,
    onQueryTable?: (tableName: string, query: string) => void,
  ): m.Children {
    return [
      // Description
      table.description &&
        m('.pf-simple-table-list__description', table.description),

      m(
        '.pf-simple-table-list__detail-row',
        m('span.pf-simple-table-list__detail-label', 'Table name'),
        // Ellipsis-truncated; tooltip reveals the full name without copying.
        m(
          'code.pf-simple-table-list__detail-value',
          {title: table.name},
          table.name,
        ),
        m(CopyToClipboardButton, {
          className: 'pf-show-on-hover',
          textToCopy: table.name,
          tooltip: 'Copy table name to clipboard',
        }),
        onQueryTable &&
          m(Button, {
            className: 'pf-show-on-hover',
            icon: 'play_arrow',
            compact: true,
            tooltip: `SELECT * FROM ${table.name} in a new tab`,
            onclick: () => onQueryTable(table.name, this.generateQuery(table)),
          }),
      ),
      // Module
      table.includeKey && this.renderIncludeRow(table.includeKey),

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
                m(Icon, {
                  icon: perfettoSqlTypeIcon(col.type),
                  className: 'pf-simple-table-list__column-icon',
                }),
                m(
                  '.pf-simple-table-list__column-info',
                  m(
                    '.pf-simple-table-list__column-header',
                    m('code.pf-simple-table-list__column-name', col.name),
                    m(
                      '.pf-simple-table-list__column-copy',
                      m(CopyToClipboardButton, {
                        textToCopy: col.name,
                        compact: true,
                        tooltip: 'Copy column name to clipboard',
                      }),
                    ),
                    m(
                      'span.pf-simple-table-list__column-type',
                      perfettoSqlTypeToString(col.type),
                    ),
                  ),
                  col.description &&
                    m('.pf-simple-table-list__column-desc', col.description),
                ),
              ),
            ),
          ),
        ),
    ];
  }
}
