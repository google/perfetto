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
import {
  SqlModules,
  SqlFunction,
  SqlModule,
} from '../../dev.perfetto.SqlModules/sql_modules';
import {Card, CardStack} from '../../../widgets/card';
import {EmptyState} from '../../../widgets/empty_state';
import {Chip} from '../../../widgets/chip';
import {classNames} from '../../../base/classnames';
import {Intent} from '../../../widgets/common';
import markdownit from 'markdown-it';

// Create a markdown renderer instance
const md = markdownit();

// A function with its module information
export interface FunctionWithModule {
  fn: SqlFunction;
  module: SqlModule;
}

// Attributes for the FunctionList component
export interface FunctionListAttrs {
  sqlModules: SqlModules;
  onFunctionClick: (fn: FunctionWithModule) => void;
  searchQuery: string;
  onSearchQueryChange: (query: string) => void;
  autofocus?: boolean;
  selectedFunction?: string;
}

// Type of match when searching for functions
type MatchType = 'function-name' | 'function-description' | 'arg-name';

// Helper function to get the display label for match types
function getMatchTypeLabel(matchType: MatchType): string | undefined {
  switch (matchType) {
    case 'function-name':
      return undefined;
    case 'function-description':
      return 'from description';
    case 'arg-name':
      return 'from argument name';
  }
}

// Renders a search input bar
class SearchBar
  implements
    m.ClassComponent<{
      query: string;
      onQueryChange: (query: string) => void;
      autofocus?: boolean;
    }>
{
  view({
    attrs,
  }: m.CVnode<{
    query: string;
    onQueryChange: (query: string) => void;
    autofocus?: boolean;
  }>) {
    return m('input[type=text].pf-search', {
      placeholder: 'Search functions...',
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

// Renders a single function card
class FunctionCard
  implements
    m.ClassComponent<{
      functionWithModule: FunctionWithModule;
      segments: FuzzySegment[];
      matchType: MatchType;
      onFunctionClick: (fn: FunctionWithModule) => void;
      isSelected: boolean;
    }>
{
  view({
    attrs,
  }: m.CVnode<{
    functionWithModule: FunctionWithModule;
    segments: FuzzySegment[];
    matchType: MatchType;
    onFunctionClick: (fn: FunctionWithModule) => void;
    isSelected: boolean;
  }>) {
    const {
      functionWithModule,
      segments,
      matchType,
      onFunctionClick,
      isSelected,
    } = attrs;
    const {fn, module} = functionWithModule;

    const renderedName = segments.map((segment) =>
      m(segment.matching ? 'strong' : 'span', segment.value),
    );

    const matchTypeLabel = getMatchTypeLabel(matchType);
    const packageName = module.includeKey.split('.')[0];

    // Format arguments for display
    const argsDisplay =
      fn.args.length > 0
        ? fn.args.map((arg) => `${arg.name}: ${arg.type}`).join(', ')
        : 'no arguments';

    return m(
      Card,
      {
        onclick: () => onFunctionClick(functionWithModule),
        interactive: true,
        className: classNames(isSelected && 'pf-selected-function'),
      },
      m(
        '.pf-function-card',
        m(
          '.pf-function-card-header',
          m('.pf-function-name', renderedName),
          matchTypeLabel &&
            m(Chip, {
              label: matchTypeLabel,
              compact: true,
              className: classNames('pf-match-type-chip'),
            }),
        ),
        packageName !== 'prelude' &&
          m('.pf-function-module', module.includeKey),
        m('.pf-function-signature', `(${argsDisplay}) → ${fn.returnType}`),
        fn.description &&
          m('.pf-function-description', m.trust(md.render(fn.description))),
      ),
    );
  }
}

// The main FunctionList component
export class FunctionList implements m.ClassComponent<FunctionListAttrs> {
  private selectedTags: Set<string> = new Set();

  view({attrs}: m.CVnode<FunctionListAttrs>) {
    const allModules = attrs.sqlModules.listModules();

    // Get all functions from all modules, excluding private functions (starting with _)
    const allFunctions: FunctionWithModule[] = allModules.flatMap((module) =>
      module.functions
        .filter((fn) => !fn.name.startsWith('_'))
        .map((fn) => ({fn, module})),
    );

    // Collect all unique tags from modules that have functions
    const allTagsSet = new Set<string>();
    for (const module of allModules) {
      if (module.functions.some((fn) => !fn.name.startsWith('_'))) {
        for (const tag of module.tags) {
          allTagsSet.add(tag);
        }
      }
    }
    const allTags = Array.from(allTagsSet).sort();

    // Filter by selected tags
    let filteredFunctions = allFunctions;
    if (this.selectedTags.size > 0) {
      filteredFunctions = allFunctions.filter((item) =>
        Array.from(this.selectedTags).every((tag) =>
          item.module.tags.includes(tag),
        ),
      );
    }

    // Search functions
    const searchFunctions = (
      functions: FunctionWithModule[],
      query: string,
    ): Array<{
      item: FunctionWithModule;
      segments: FuzzySegment[];
      matchType: MatchType;
    }> => {
      if (query.trim() === '') {
        return functions.map((item) => ({
          item,
          segments: [{matching: false, value: item.fn.name}],
          matchType: 'function-name' as MatchType,
        }));
      }

      const matchedNames = new Set<string>();
      const lowerQuery = query.toLowerCase();

      // 1. Search by function name (fuzzy)
      const nameFinder = new FuzzyFinder(functions, (item) => item.fn.name);
      const nameResults = nameFinder.find(query).map((result) => {
        matchedNames.add(result.item.fn.name);
        return {
          ...result,
          matchType: 'function-name' as MatchType,
        };
      });

      // 2. Search by description (exact)
      const descResults: Array<{
        item: FunctionWithModule;
        segments: FuzzySegment[];
        matchType: MatchType;
      }> = [];
      for (const item of functions) {
        if (matchedNames.has(item.fn.name)) continue;
        if (
          item.fn.description &&
          item.fn.description.toLowerCase().includes(lowerQuery)
        ) {
          matchedNames.add(item.fn.name);
          descResults.push({
            item,
            segments: [{matching: false, value: item.fn.name}],
            matchType: 'function-description',
          });
        }
      }

      // 3. Search by argument names (exact)
      const argResults: Array<{
        item: FunctionWithModule;
        segments: FuzzySegment[];
        matchType: MatchType;
      }> = [];
      for (const item of functions) {
        if (matchedNames.has(item.fn.name)) continue;
        const hasMatch = item.fn.args.some((arg) =>
          arg.name.toLowerCase().includes(lowerQuery),
        );
        if (hasMatch) {
          matchedNames.add(item.fn.name);
          argResults.push({
            item,
            segments: [{matching: false, value: item.fn.name}],
            matchType: 'arg-name',
          });
        }
      }

      return [...nameResults, ...descResults, ...argResults];
    };

    const searchResults = searchFunctions(filteredFunctions, attrs.searchQuery);

    // Compute disabled tags
    const disabledTags = new Set<string>();
    for (const tag of allTags) {
      if (!this.selectedTags.has(tag)) {
        const testTags = new Set(this.selectedTags);
        testTags.add(tag);
        const wouldMatch = allFunctions.filter((item) =>
          Array.from(testTags).every((t) => item.module.tags.includes(t)),
        );
        if (wouldMatch.length === 0) {
          disabledTags.add(tag);
        } else if (attrs.searchQuery.trim() !== '') {
          const searchRes = searchFunctions(wouldMatch, attrs.searchQuery);
          if (searchRes.length === 0) {
            disabledTags.add(tag);
          }
        }
      }
    }

    const functionCards = searchResults.map(({item, segments, matchType}) =>
      m(FunctionCard, {
        functionWithModule: item,
        segments,
        matchType,
        onFunctionClick: attrs.onFunctionClick,
        isSelected: item.fn.name === attrs.selectedFunction,
      }),
    );

    return m(
      '.pf-exp-function-list',
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
                    if (isDisabled) return;
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
      ),
      m(
        '.pf-function-cards-container',
        functionCards.length > 0
          ? m(CardStack, functionCards)
          : m(EmptyState, {title: 'No functions found'}),
      ),
    );
  }
}
