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
import {
  fuzzySearch,
  type FuzzyResult,
  type FuzzySegment,
} from '../../../base/fuzzy';
import {
  type SqlModules,
  type SqlTable,
  isTableEffectivelyDisabled,
} from '../../dev.perfetto.SqlModules/sql_modules';
import {Card, CardStack} from '../../../widgets/card';
import {EmptyState} from '../../../widgets/empty_state';
import {Chip} from '../../../widgets/chip';
import {classNames} from '../../../base/classnames';
import {Intent} from '../../../widgets/common';
import {Switch} from '../../../widgets/switch';
import {Icon} from '../../../widgets/icon';
import {Tooltip} from '../../../widgets/tooltip';
import markdownit from 'markdown-it';

// Create a markdown renderer instance
const md = markdownit();

// Attributes for the main TableList component.
export interface TableListAttrs {
  sqlModules: SqlModules;
  onTableClick: (tableName: string, event: MouseEvent) => void;
  searchQuery: string;
  onSearchQueryChange: (query: string) => void;
  autofocus?: boolean;
  selectedTables?: Set<string>;
}

// A helper interface that combines a SQL table with its module name.
interface TableWithModule {
  table: SqlTable;
  moduleName: string;
}

// Type of match when searching for tables
type MatchType =
  'table-name' | 'column-name' | 'table-description' | 'column-description';

// Helper function to get the display label for importance levels.
function getImportanceLabel(
  importance: 'core' | 'high' | 'mid' | 'low',
): string {
  switch (importance) {
    case 'core':
      return 'Core';
    case 'high':
      return 'Very common';
    case 'mid':
      return 'Common';
    case 'low':
      return 'Deprecated';
  }
}

// Helper function to check if a table has timestamp columns.
function isTimestampedTable(table: SqlTable): boolean {
  return table.columns.some((col) => col.type?.kind === 'timestamp');
}

// Renders a search input bar.
// This component is responsible for handling user input for searching
// and communicating the query back to the parent component.
class SearchBar implements m.ClassComponent<{
  query: string;
  onQueryChange: (query: string) => void;
  autofocus?: boolean;
  placeholder?: string;
}> {
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

// Helper function to get the display label for match types.
function getMatchTypeLabel(matchType: MatchType): string | undefined {
  switch (matchType) {
    case 'table-name':
      return undefined; // No label needed for table name matches
    case 'column-name':
      return 'from column name';
    case 'table-description':
      return 'from table description';
    case 'column-description':
      return 'from column description';
  }
}

// Renders a single table card in the list.
// This component displays the table name, its module, and description.
// It also highlights the parts of the name that match the search query.
class TableCard implements m.ClassComponent<{
  tableWithModule: TableWithModule;
  segments: readonly FuzzySegment[];
  matchType: MatchType;
  onTableClick: (tableName: string, event: MouseEvent) => void;
  sqlModules: SqlModules;
  selectedTables?: Set<string>;
}> {
  view({
    attrs,
  }: m.CVnode<{
    tableWithModule: TableWithModule;
    segments: readonly FuzzySegment[];
    matchType: MatchType;
    onTableClick: (tableName: string, event: MouseEvent) => void;
    sqlModules: SqlModules;
    selectedTables?: Set<string>;
  }>) {
    const {
      tableWithModule,
      segments,
      matchType,
      onTableClick,
      sqlModules,
      selectedTables,
    } = attrs;
    const {table, moduleName} = tableWithModule;

    const renderedName = segments.map((segment) =>
      m(segment.matching ? 'strong' : 'span', segment.value),
    );

    const packageName = moduleName.split('.')[0];
    const matchTypeLabel = getMatchTypeLabel(matchType);
    const isDisabled = isTableEffectivelyDisabled(sqlModules, table.name);
    const isSelected = selectedTables?.has(table.name) ?? false;

    const hasTimestamp = isTimestampedTable(table);

    return m(
      Card,
      {
        onclick: (e: MouseEvent) => onTableClick(table.name, e),
        interactive: true,
        className: classNames(
          isDisabled && 'pf-disabled-module',
          isSelected && 'pf-selected-table',
        ),
      },
      m(
        '.pf-table-card',
        m(
          '.pf-table-card-header',
          m('.table-name', renderedName),
          hasTimestamp &&
            m(
              Tooltip,
              {
                trigger: m(Icon, {
                  icon: 'schedule',
                  className: classNames('pf-timestamp-icon'),
                }),
              },
              'This table contains timestamp columns',
            ),
          matchTypeLabel &&
            m(Chip, {
              label: matchTypeLabel,
              compact: true,
              className: classNames('pf-match-type-chip'),
            }),
          isDisabled &&
            m(Chip, {
              label: 'No data',
              compact: true,
              intent: Intent.None,
              className: classNames('pf-no-data-chip'),
            }),
          table.importance &&
            table.importance !== 'mid' &&
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
        table.description &&
          m('.table-description', m.trust(md.render(table.description))),
      ),
    );
  }
}

interface SearchResult extends FuzzyResult<TableWithModule> {
  readonly matchType: MatchType;
}

// Searches tables by query. Returns results in priority order: table name,
// column name, table description, column description.
export function searchTables(
  tables: ReadonlyArray<TableWithModule>,
  query: string,
): SearchResult[] {
  if (query.trim() === '') {
    return tables.map((table) => ({
      item: table,
      segments: [{matching: false, value: table.table.name}],
      matchType: 'table-name' as MatchType,
      score: 1,
    }));
  }

  // Track which tables have been matched to avoid duplicates
  const matchedTableNames = new Set<string>();

  // 1. Search by table name (highest priority)
  const tableNameResults = fuzzySearch(
    tables,
    (item) => item.table.name,
    query,
  ).map((result) => {
    matchedTableNames.add(result.item.table.name);
    return {
      ...result,
      matchType: 'table-name' as MatchType,
    };
  });

  // 2. Search by column names (second priority) - exact match
  const columnNameResults: SearchResult[] = [];

  const lowerQuery = query.toLowerCase();
  for (const tableWithModule of tables) {
    if (matchedTableNames.has(tableWithModule.table.name)) {
      continue;
    }

    const hasMatch = tableWithModule.table.columns.some((col) =>
      col.name.toLowerCase().includes(lowerQuery),
    );

    if (hasMatch) {
      matchedTableNames.add(tableWithModule.table.name);
      columnNameResults.push({
        item: tableWithModule,
        segments: [{matching: false, value: tableWithModule.table.name}],
        matchType: 'column-name',
        score: 0,
      });
    }
  }

  // 3. Search by table description (third priority) - exact match
  const tableDescriptionResults: SearchResult[] = [];

  for (const tableWithModule of tables) {
    if (matchedTableNames.has(tableWithModule.table.name)) {
      continue;
    }

    if (
      tableWithModule.table.description &&
      tableWithModule.table.description.toLowerCase().includes(lowerQuery)
    ) {
      matchedTableNames.add(tableWithModule.table.name);
      tableDescriptionResults.push({
        item: tableWithModule,
        segments: [{matching: false, value: tableWithModule.table.name}],
        matchType: 'table-description',
        score: 0,
      });
    }
  }

  // 4. Search by column descriptions (lowest priority) - exact match
  const columnDescriptionResults: SearchResult[] = [];

  for (const tableWithModule of tables) {
    if (matchedTableNames.has(tableWithModule.table.name)) {
      continue;
    }

    const hasMatch = tableWithModule.table.columns.some(
      (col) =>
        col.description !== undefined &&
        col.description.toLowerCase().includes(lowerQuery),
    );

    if (hasMatch) {
      matchedTableNames.add(tableWithModule.table.name);
      columnDescriptionResults.push({
        item: tableWithModule,
        segments: [{matching: false, value: tableWithModule.table.name}],
        matchType: 'column-description',
        score: 0,
      });
    }
  }

  return [
    ...tableNameResults,
    ...columnNameResults,
    ...tableDescriptionResults,
    ...columnDescriptionResults,
  ];
}

// Rounds a fuzzy relevance score into coarse buckets. Matches whose scores land
// in the same bucket are treated as equally relevant and are then ordered by
// importance. Larger buckets let importance decide more matches; smaller buckets
// let fuzzy relevance dominate.
const FUZZY_SCORE_BUCKET = 0.1;

function fuzzyBucket(score: number): number {
  return Math.round(score / FUZZY_SCORE_BUCKET);
}

// Importance as a sortable rank, higher is more important. Tables with no
// declared importance sit between 'mid' and 'low', matching the previous order.
const IMPORTANCE_RANK = {core: 4, high: 3, mid: 2, low: 0} as const;
const DEFAULT_IMPORTANCE_RANK = 1;

function importanceRank(importance?: 'core' | 'high' | 'mid' | 'low'): number {
  return importance === undefined
    ? DEFAULT_IMPORTANCE_RANK
    : IMPORTANCE_RANK[importance];
}

// Ranks a group of matches lexicographically: bucketed fuzzy relevance first,
// importance second. So a clearly better match always wins, and importance only
// decides the order between matches of comparable relevance. The substring match
// groups carry no fuzzy score, so this reduces to ordering them by importance.
function rankByRelevance(results: ReadonlyArray<SearchResult>): SearchResult[] {
  return [...results].sort((a, b) => {
    const bucketDiff = fuzzyBucket(b.score) - fuzzyBucket(a.score);
    if (bucketDiff !== 0) return bucketDiff;
    return (
      importanceRank(b.item.table.importance) -
      importanceRank(a.item.table.importance)
    );
  });
}

// Searches and ranks tables for display. Within each match group the order is
// bucketed fuzzy relevance first, importance second, so that for example the
// query `androidx_` surfaces the exact extension-server matches
// (`androidx_art_metrics`) above the merely-fuzzy stdlib matches
// (`android_frames`), while more important tables still come first among matches
// of comparable relevance.
export function searchAndRankTables(
  tables: ReadonlyArray<TableWithModule>,
  query: string,
): SearchResult[] {
  const searchResults = searchTables(tables, query);

  const rankGroup = (matchType: MatchType) =>
    rankByRelevance(searchResults.filter((r) => r.matchType === matchType));

  return [
    ...rankGroup('table-name'),
    ...rankGroup('column-name'),
    ...rankGroup('table-description'),
    ...rankGroup('column-description'),
  ];
}

// The main component that displays a searchable list of SQL tables.
// It orchestrates the search bar, the list of tables, and handles filtering.
export class TableList implements m.ClassComponent<TableListAttrs> {
  private selectedTags: Set<string> = new Set();
  private hideDisabledModules: boolean = true;
  private onlyShowTimestampedTables: boolean = false;

  view({attrs}: m.CVnode<TableListAttrs>) {
    const allModules = attrs.sqlModules.listModules();

    // Collect all unique tags from modules that have at least one table
    const allTagsSet = new Set<string>();
    for (const module of allModules) {
      if (module.tables.length > 0) {
        // Skip disabled modules when collecting tags if hideDisabledModules is enabled
        if (
          this.hideDisabledModules &&
          attrs.sqlModules.isModuleDisabled(module.includeKey)
        ) {
          continue;
        }
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

    // Filter out disabled tables if hideDisabledModules is true
    if (this.hideDisabledModules) {
      filteredModules = filteredModules
        .map((module) => ({
          ...module,
          tables: module.tables.filter(
            (table) =>
              !isTableEffectivelyDisabled(attrs.sqlModules, table.name),
          ),
        }))
        .filter((module) => module.tables.length > 0);
    }

    // Filter to only timestamped tables if onlyShowTimestampedTables is true
    if (this.onlyShowTimestampedTables) {
      filteredModules = filteredModules
        .map((module) => ({
          ...module,
          tables: module.tables.filter(isTimestampedTable),
        }))
        .filter((module) => module.tables.length > 0);
    }

    const allTables: TableWithModule[] = filteredModules.flatMap((module) =>
      module.tables.map((table) => ({table, moduleName: module.includeKey})),
    );

    // Compute which tags should be disabled
    // A tag should be disabled if selecting it (in addition to current tags)
    // would result in 0 tables OR if combined with the search query would result in 0 results
    const disabledTags = new Set<string>();
    for (const tag of allTags) {
      if (!this.selectedTags.has(tag)) {
        // Compute what modules would exist if this tag was selected
        const testSelectedTags = new Set(this.selectedTags);
        testSelectedTags.add(tag);

        const modulesWithThisTag = allModules.filter((module) =>
          Array.from(testSelectedTags).every((selectedTag) =>
            module.tags.includes(selectedTag),
          ),
        );

        const tablesWithThisTag: TableWithModule[] = modulesWithThisTag.flatMap(
          (module) =>
            module.tables.map((table) => ({
              table,
              moduleName: module.includeKey,
            })),
        );

        // Check if there would be any tables
        if (tablesWithThisTag.length === 0) {
          disabledTags.add(tag);
          continue;
        }

        // If there's a search query, check if any tables would match the search
        if (attrs.searchQuery.trim() !== '') {
          const searchResults = searchTables(
            tablesWithThisTag,
            attrs.searchQuery,
          );
          if (searchResults.length === 0) {
            disabledTags.add(tag);
          }
        }
      }
    }

    // Perform the actual search for display
    const sortedFuzzyResults = searchAndRankTables(
      allTables,
      attrs.searchQuery,
    );

    const tableCards = sortedFuzzyResults.map(({item, segments, matchType}) =>
      m(TableCard, {
        tableWithModule: item,
        segments,
        matchType,
        onTableClick: attrs.onTableClick,
        sqlModules: attrs.sqlModules,
        selectedTables: attrs.selectedTables,
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
      m(
        '.pf-search-and-filter',
        m(SearchBar, {
          query: attrs.searchQuery,
          onQueryChange: attrs.onSearchQueryChange,
          autofocus: attrs.autofocus,
        }),
        m(
          '.pf-search-and-filter-switches',
          m(Switch, {
            label: 'Hide modules with no data',
            checked: this.hideDisabledModules,
            onchange: () => {
              this.hideDisabledModules = !this.hideDisabledModules;
            },
          }),
          m(Switch, {
            label: 'Only show timestamped tables',
            checked: this.onlyShowTimestampedTables,
            onchange: () => {
              this.onlyShowTimestampedTables = !this.onlyShowTimestampedTables;
            },
          }),
        ),
      ),
      m(
        '.pf-table-cards-container',
        m(
          CardStack,
          tableCards.length > 0
            ? m(CardStack, tableCards)
            : m(EmptyState, {title: 'No tables found'}),
        ),
      ),
    );
  }
}
