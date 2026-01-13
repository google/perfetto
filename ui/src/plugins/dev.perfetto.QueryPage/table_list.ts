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
import {FuzzyFinder, FuzzySegment} from '../../base/fuzzy';
import {Accordion, AccordionItem} from '../../widgets/accordion';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {Icon} from '../../widgets/icon';
import {TextInput} from '../../widgets/text_input';
import {SqlModules, SqlTable} from '../dev.perfetto.SqlModules/sql_modules';
import {
  PerfettoSqlType,
  perfettoSqlTypeToString,
} from '../../trace_processor/perfetto_sql_type';
import {EmptyState} from '../../widgets/empty_state';

interface FilteredTable {
  table: SqlTable;
  segments: FuzzySegment[];
}

function renderHighlightedName(segments: FuzzySegment[]): m.Children {
  return segments.map(({matching, value}) =>
    matching ? m('span.pf-simple-table-list__highlight', value) : value,
  );
}

// Returns a Material icon name for a given PerfettoSQL type
function getTypeIcon(type?: PerfettoSqlType): string {
  if (type === undefined) return 'help_outline';
  switch (type.kind) {
    case 'int':
      return 'tag';
    case 'double':
      return 'decimal_increase';
    case 'string':
      return 'text_fields';
    case 'boolean':
      return 'toggle_on';
    case 'timestamp':
      return 'schedule';
    case 'duration':
      return 'timer';
    case 'bytes':
      return 'memory';
    case 'id':
    case 'joinid':
      return 'key';
    case 'arg_set_id':
      return 'dataset';
  }
}

export interface TableListAttrs {
  readonly sqlModules: SqlModules;
}

export class TableList implements m.ClassComponent<TableListAttrs> {
  private searchQuery = '';
  private expandedTable: string | undefined = undefined;

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

    const items: AccordionItem[] = filteredTables.map(({table, segments}) => ({
      id: table.name,
      header: m(
        'code.pf-simple-table-list__item-name',
        renderHighlightedName(segments),
      ),
      content: this.renderTableContent(table),
    }));

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
      items.length > 0
        ? m(
            '.pf-simple-table-list__items',
            m(Accordion, {
              items,
              expanded: this.expandedTable,
              onToggle: (id) => {
                this.expandedTable = id;
              },
            }),
          )
        : m(EmptyState, {
            title: 'No matching tables found',
          }),
    );
  }

  private renderTableContent(table: SqlTable): m.Children {
    return [
      // Description
      table.description &&
        m('.pf-simple-table-list__description', table.description),

      m(
        '.pf-simple-table-list__detail-row',
        m('span.pf-simple-table-list__detail-label', 'Table name'),
        m('code.pf-simple-table-list__detail-value', table.name),
        m(CopyToClipboardButton, {
          className: 'pf-show-on-hover',
          textToCopy: table.name,
          tooltip: 'Copy table name to clipboard',
        }),
      ),
      // Module
      table.includeKey &&
        m(
          '.pf-simple-table-list__detail-row',
          m('span.pf-simple-table-list__detail-label', 'Include'),
          m(
            'code.pf-simple-table-list__detail-value',
            `INCLUDE PERFETTO MODULE ${table.includeKey};`,
          ),
          m(CopyToClipboardButton, {
            className: 'pf-show-on-hover',
            textToCopy: `INCLUDE PERFETTO MODULE ${table.includeKey};`,
            tooltip: 'Copy include string to clipboard',
          }),
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
                m(Icon, {
                  icon: getTypeIcon(col.type),
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
