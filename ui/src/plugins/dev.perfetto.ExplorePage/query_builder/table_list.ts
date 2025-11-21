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
import {FuzzyFinder, FuzzySegment} from '../../../base/fuzzy';
import {SqlModules, SqlTable} from '../../dev.perfetto.SqlModules/sql_modules';
import {Card, CardStack} from '../../../widgets/card';
import {EmptyState} from '../../../widgets/empty_state';
import {Chip} from '../../../widgets/chip';
import {classNames} from '../../../base/classnames';
import {Intent} from '../../../widgets/common';

// Attributes for the main TableList component.
export interface TableListAttrs {
  sqlModules: SqlModules;
  onTableClick: (tableName: string) => void;
  searchQuery: string;
  onSearchQueryChange: (query: string) => void;
  autofocus?: boolean;
}

// A helper interface that combines a SQL table with its module name.
interface TableWithModule {
  table: SqlTable;
  moduleName: string;
}

// Renders a search input bar.
// This component is responsible for handling user input for searching
// and communicating the query back to the parent component.
class SearchBar
  implements
    m.ClassComponent<{
      query: string;
      onQueryChange: (query: string) => void;
      autofocus?: boolean;
      placeholder?: string;
    }>
{
  view({
    attrs,
  }: m.CVnode<{
    query: string;
    onQueryChange: (query: string) => void;
    autofocus?: boolean;
    placeholder?: string;
  }>) {
    return m('input[type=text].pf-search', {
      placeholder: attrs.placeholder ?? 'Search Perfetto SQL tables...',
      oninput: (e: Event) => {
        attrs.onQueryChange((e.target as HTMLInputElement).value);
      },
      value: attrs.query,
      oncreate: (vnode) => {
        if (attrs.autofocus) {
          (vnode.dom as HTMLInputElement).focus();
        }
      },
    });
  }
}

// Renders a single table card in the list.
// This component displays the table name, its module, and description.
// It also highlights the parts of the name that match the search query.
class TableCard
  implements
    m.ClassComponent<{
      tableWithModule: TableWithModule;
      segments: FuzzySegment[];
      onTableClick: (tableName: string) => void;
    }>
{
  view({
    attrs,
  }: m.CVnode<{
    tableWithModule: TableWithModule;
    segments: FuzzySegment[];
    onTableClick: (tableName: string) => void;
  }>) {
    const {tableWithModule, segments, onTableClick} = attrs;
    const {table, moduleName} = tableWithModule;

    const renderedName = segments.map((segment) =>
      m(segment.matching ? 'strong' : 'span', segment.value),
    );

    const packageName = moduleName.split('.')[0];

    return m(
      Card,
      {
        onclick: () => onTableClick(table.name),
        interactive: true,
      },
      m(
        '.pf-table-card',
        m('.table-name', renderedName),
        packageName === 'prelude' ? null : m('.table-module', moduleName),
        table.description && m('.table-description', table.description),
      ),
    );
  }
}

// The main component that displays a searchable list of SQL tables.
// It orchestrates the search bar, the list of tables, and handles filtering.
export class TableList implements m.ClassComponent<TableListAttrs> {
  private selectedTags: Set<string> = new Set();

  view({attrs}: m.CVnode<TableListAttrs>) {
    const allModules = attrs.sqlModules.listModules();

    // Collect all unique tags from all modules
    const allTagsSet = new Set<string>();
    for (const module of allModules) {
      for (const tag of module.tags) {
        allTagsSet.add(tag);
      }
    }
    const allTags = Array.from(allTagsSet).sort();

    // Filter tables by selected tags if any are selected (AND behavior)
    let filteredModules = allModules;
    if (this.selectedTags.size > 0) {
      filteredModules = allModules.filter((module) =>
        Array.from(this.selectedTags).every((selectedTag) =>
          module.tags.includes(selectedTag),
        ),
      );
    }

    const allTables: TableWithModule[] = filteredModules.flatMap((module) =>
      module.tables.map((table) => ({table, moduleName: module.includeKey})),
    );

    const finder = new FuzzyFinder(allTables, (item) => item.table.name);
    const fuzzyResults = finder.find(attrs.searchQuery);

    const tableCards = fuzzyResults.map(({item, segments}) =>
      m(TableCard, {
        tableWithModule: item,
        segments,
        onTableClick: attrs.onTableClick,
      }),
    );

    return m(
      '.pf-exp-table-list',
      // Tag filter section
      allTags.length > 0
        ? m(
            '.pf-tag-filter',
            m(
              '.pf-tag-filter-chips',
              allTags.map((tag) => {
                const isSelected = this.selectedTags.has(tag);
                return m(Chip, {
                  label: tag,
                  rounded: true,
                  intent: isSelected ? Intent.Primary : undefined,
                  className: classNames(
                    'pf-tag-chip',
                    isSelected && 'pf-tag-chip-selected',
                  ),
                  onclick: () => {
                    if (isSelected) {
                      this.selectedTags.delete(tag);
                    } else {
                      this.selectedTags.add(tag);
                    }
                  },
                });
              }),
            ),
          )
        : null,
      m(SearchBar, {
        query: attrs.searchQuery,
        onQueryChange: attrs.onSearchQueryChange,
        autofocus: attrs.autofocus,
      }),
      m(
        CardStack,
        tableCards.length > 0
          ? m(CardStack, tableCards)
          : m(EmptyState, {title: 'No tables found'}),
      ),
    );
  }
}
