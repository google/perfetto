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
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {Icon} from '../../widgets/icon';
import {TextInput} from '../../widgets/text_input';
import {SqlModules, SqlTable} from '../dev.perfetto.SqlModules/sql_modules';
import {
  PerfettoSqlType,
  perfettoSqlTypeToString,
} from '../../trace_processor/perfetto_sql_type';

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
        filteredTables.map((ft) => this.renderTableItem(ft)),
      ),
    );
  }

  private renderTableItem({table, segments}: FilteredTable): m.Children {
    const isExpanded = this.expandedTable === table.name;

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
              // Accordion: toggle off if already expanded, otherwise expand this one
              this.expandedTable = isExpanded ? undefined : table.name;
            },
          },
          m(Icon, {icon: isExpanded ? 'expand_more' : 'chevron_right'}),
        ),
        m('.pf-simple-table-list__item-name', renderHighlightedName(segments)),
        m(CopyToClipboardButton, {
          textToCopy: table.name,
          compact: true,
          tooltip: 'Copy table name to clipboard',
        }),
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
              m(CopyToClipboardButton, {
                textToCopy: `INCLUDE PERFETTO MODULE ${table.includeKey};`,
                compact: true,
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
                    m('code.pf-simple-table-list__column-name', col.name),
                    m(
                      'span.pf-simple-table-list__column-type',
                      perfettoSqlTypeToString(col.type),
                    ),
                  ),
                ),
              ),
            ),
        ),
    );
  }
}
