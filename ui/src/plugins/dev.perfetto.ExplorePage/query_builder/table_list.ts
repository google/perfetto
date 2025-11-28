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

// Helper function to get the display label for importance levels.
function getImportanceLabel(importance: 'high' | 'mid' | 'low'): string {
  switch (importance) {
    case 'high':
      return 'Important';
    case 'mid':
      return 'Recommended';
    case 'low':
      return 'Low';
  }
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
      placeholder: attrs.placeholder ?? 'Search tables...',
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
        m(
          '.pf-table-card-header',
          m('.table-name', renderedName),
          table.importance &&
            m(Chip, {
              label: getImportanceLabel(table.importance),
              compact: true,
              className: classNames(
                'pf-importance-chip',
                `pf-importance-${table.importance}`,
              ),
            }),
        ),
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

    // Collect all unique tags from modules that have at least one table
    const allTagsSet = new Set<string>();
    for (const module of allModules) {
      if (module.tables.length > 0) {
        for (const tag of module.tags) {
          allTagsSet.add(tag);
        }
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

    // Compute which tags should be disabled
    // A tag should be disabled if selecting it (in addition to current tags) would result in 0 tables
    const disabledTags = new Set<string>();
    if (this.selectedTags.size > 0) {
      for (const tag of allTags) {
        if (!this.selectedTags.has(tag)) {
          // Check if adding this tag to selected tags would result in any modules
          const wouldHaveModules = allModules.some((module) => {
            // Module must have all currently selected tags AND this tag
            const hasAllSelectedTags = Array.from(this.selectedTags).every(
              (selectedTag) => module.tags.includes(selectedTag),
            );
            const hasThisTag = module.tags.includes(tag);
            const hasTables = module.tables.length > 0;
            return hasAllSelectedTags && hasThisTag && hasTables;
          });

          if (!wouldHaveModules) {
            disabledTags.add(tag);
          }
        }
      }
    }

    const allTables: TableWithModule[] = filteredModules.flatMap((module) =>
      module.tables.map((table) => ({table, moduleName: module.includeKey})),
    );

    const finder = new FuzzyFinder(allTables, (item) => item.table.name);
    const fuzzyResults = finder.find(attrs.searchQuery);

    // Group fuzzy results by importance level to ensure:
    // - High importance tables always appear first
    // - Low importance tables always appear last
    // - Within each level, fuzzy finder's natural sorting applies
    const highImportance = fuzzyResults.filter(
      (r) => r.item.table.importance === 'high',
    );
    const midImportance = fuzzyResults.filter(
      (r) => r.item.table.importance === 'mid',
    );
    const normalImportance = fuzzyResults.filter(
      (r) => r.item.table.importance === undefined,
    );
    const lowImportance = fuzzyResults.filter(
      (r) => r.item.table.importance === 'low',
    );

    const sortedFuzzyResults = [
      ...highImportance,
      ...midImportance,
      ...normalImportance,
      ...lowImportance,
    ];

    const tableCards = sortedFuzzyResults.map(({item, segments}) =>
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
                const isDisabled = disabledTags.has(tag);
                return m(Chip, {
                  label: tag,
                  rounded: true,
                  intent: isSelected ? Intent.Primary : undefined,
                  className: classNames(
                    'pf-tag-chip',
                    isSelected && 'pf-tag-chip-selected',
                  ),
                  disabled: isDisabled,
                  onclick: () => {
                    if (isDisabled) {
                      return;
                    }
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
