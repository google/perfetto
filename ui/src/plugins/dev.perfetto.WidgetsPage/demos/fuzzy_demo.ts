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
import {fuzzySearch, type FuzzySegment} from '../../../base/fuzzy';
import {TextInput} from '../../../widgets/text_input';
import {Checkbox} from '../../../widgets/checkbox';
import {Card, CardStack} from '../../../widgets/card';
import {Chip} from '../../../widgets/chip';
import {EmptyState} from '../../../widgets/empty_state';

interface SampleItem {
  readonly id: string;
  readonly name: string;
  readonly category: string;
}

function generateSampleItems(): SampleItem[] {
  const prefixes = [
    'dev.perfetto',
    'com.android',
    'org.chromium',
    'v8',
    'sys',
    'gpu',
  ];
  const words1 = [
    'Gpu',
    'Render',
    'Device',
    'Memory',
    'Texture',
    'Shader',
    'Buffer',
    'Queue',
    'Async',
    'Direct',
    'Shared',
    'Virtual',
    'Global',
    'Frame',
    'Slice',
    'Track',
    'XML',
    'HTTP',
    'JSON',
    'IPC',
  ];
  const words2 = [
    'Compute',
    'Command',
    'Allocator',
    'Context',
    'Manager',
    'Dispatcher',
    'Session',
    'Worker',
    'Scheduler',
    'Parser',
    'Serializer',
    'Controller',
    'Handler',
    'Engine',
  ];
  const words3 = [
    'Pipeline',
    'Encoder',
    'Pool',
    'Cache',
    'Registry',
    'Listener',
    'Service',
    'Proxy',
    'Wrapper',
    'Channel',
    'Stream',
  ];

  const items: SampleItem[] = [];
  let idCounter = 1;

  function toCamelCase(words: string[]): string {
    return (
      words[0].toLowerCase() +
      words
        .slice(1)
        .map((w) => w.charAt(0).toUpperCase() + w.slice(1))
        .join('')
    );
  }

  function toPascalCase(words: string[]): string {
    return words.map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join('');
  }

  function toSnakeCase(words: string[]): string {
    return words.map((w) => w.toLowerCase()).join('_');
  }

  function toKebabCase(words: string[]): string {
    return words.map((w) => w.toLowerCase()).join('-');
  }

  for (const w1 of words1) {
    for (const w2 of words2) {
      for (const w3 of words3.slice(0, 3)) {
        const parts = [w1, w2, w3];

        // 1. PascalCase
        items.push({
          id: String(idCounter++),
          name: toPascalCase(parts),
          category: 'PascalCase',
        });

        // 2. camelCase
        items.push({
          id: String(idCounter++),
          name: toCamelCase(parts),
          category: 'camelCase',
        });

        // 3. snake_case
        items.push({
          id: String(idCounter++),
          name: toSnakeCase(parts),
          category: 'snake_case',
        });

        // 4. Dotted namespace
        const prefix = prefixes[idCounter % prefixes.length];
        items.push({
          id: String(idCounter++),
          name: `${prefix}.${toPascalCase(parts)}`,
          category: 'Dotted',
        });

        // 5. Hyphenated / kebab-case
        if (idCounter % 5 === 0) {
          items.push({
            id: String(idCounter++),
            name: toKebabCase(parts),
            category: 'kebab-case',
          });
        }
      }
    }
  }

  return items;
}

const SAMPLE_ITEMS = generateSampleItems();

let searchTerm = '';
let includeTag = false;

export function renderFuzzyDemo(): m.Children {
  const isSearching = searchTerm.trim() !== '';
  let results: {
    item: SampleItem;
    nameSegments: readonly FuzzySegment[];
    categorySegments: readonly FuzzySegment[];
    score?: number;
  }[] = [];
  let durationMs = 0;

  if (isSearching) {
    const startTime = performance.now();
    if (includeTag) {
      const searchResults = fuzzySearch(
        SAMPLE_ITEMS,
        [(item: SampleItem) => item.name, (item: SampleItem) => item.category],
        searchTerm,
      );
      results = searchResults.map((res) => ({
        item: res.item,
        nameSegments: res.segments[0],
        categorySegments: res.segments[1],
        score: res.score,
      }));
    } else {
      const searchResults = fuzzySearch(
        SAMPLE_ITEMS,
        (item) => item.name,
        searchTerm,
      );
      results = searchResults.map((res) => ({
        item: res.item,
        nameSegments: res.segments,
        categorySegments: [{matching: false, value: res.item.category}],
        score: res.score,
      }));
    }
    durationMs = performance.now() - startTime;
  } else {
    results = SAMPLE_ITEMS.map((item) => ({
      item,
      nameSegments: [{matching: false, value: item.name}],
      categorySegments: [{matching: false, value: item.category}],
    }));
  }

  const MAX_VISIBLE = 50;
  const visibleResults = results.slice(0, MAX_VISIBLE);
  const remainingCount = results.length - visibleResults.length;

  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Fuzzy Search'),
      m(
        'p',
        'Fuzzy matching list example using FuzzyFinder over a large set of programmatically generated items. ' +
          'Type in the search box below to filter and highlight matches across various naming conventions ' +
          '(camelCase, PascalCase, snake_case, kebab-case, acronyms, dotted namespaces).',
      ),
    ),
    m(
      '.pf-fuzzy-demo',
      {style: {display: 'flex', flexDirection: 'column', gap: '12px'}},
      m(
        '.pf-fuzzy-demo-controls',
        {style: {display: 'flex', gap: '16px', alignItems: 'center'}},
        m(TextInput, {
          placeholder:
            'Try searching e.g. "gpucompute", "gpu compute", "render", "xml", "device_memory"...',
          leftIcon: 'search',
          value: searchTerm,
          oninput: (e: Event) => {
            searchTerm = (e.target as HTMLInputElement).value;
          },
          style: {flexGrow: '1'},
        }),
        m(Checkbox, {
          label: 'Also match category tag',
          checked: includeTag,
          onchange: (e: Event) => {
            includeTag = (e.target as HTMLInputElement).checked;
          },
        }),
      ),
      m(
        '.pf-fuzzy-demo-count',
        {
          style: {
            fontSize: '12px',
            color: 'var(--pf-color-text-secondary, #666)',
          },
        },
        isSearching
          ? `${results.length} of ${SAMPLE_ITEMS.length} items (${durationMs.toFixed(2)} ms)`
          : `${SAMPLE_ITEMS.length} items`,
      ),
      results.length > 0
        ? m(
            'div',
            {style: {display: 'flex', flexDirection: 'column', gap: '8px'}},
            m(
              CardStack,
              visibleResults.map((res) => {
                return m(
                  Card,
                  {key: res.item.id},
                  m(
                    '.pf-fuzzy-demo-item',
                    {
                      style: {
                        display: 'flex',
                        justifyContent: 'space-between',
                        alignItems: 'center',
                      },
                    },
                    m(
                      '.pf-fuzzy-demo-name',
                      res.nameSegments.map((seg) =>
                        seg.matching
                          ? m(
                              'strong',
                              {
                                style: {
                                  fontWeight: 'bold',
                                  color: 'var(--pf-color-primary, #0055ff)',
                                },
                              },
                              seg.value,
                            )
                          : m('span', seg.value),
                      ),
                    ),
                    m(
                      '.pf-fuzzy-demo-meta',
                      {
                        style: {
                          display: 'flex',
                          gap: '8px',
                          alignItems: 'center',
                        },
                      },
                      isSearching &&
                        res.score !== undefined &&
                        m(
                          'span.pf-fuzzy-demo-score',
                          {
                            style: {
                              fontSize: '11px',
                              fontFamily: 'monospace',
                              color: 'var(--pf-color-text-secondary, #777)',
                            },
                          },
                          `score: ${res.score.toFixed(3)}`,
                        ),
                      m(Chip, {
                        compact: true,
                        label: res.categorySegments.map((seg) =>
                          seg.matching
                            ? m(
                                'strong',
                                {
                                  style: {
                                    fontWeight: 'bold',
                                    color: 'var(--pf-color-primary, #0055ff)',
                                  },
                                },
                                seg.value,
                              )
                            : m('span', seg.value),
                        ),
                      }),
                    ),
                  ),
                );
              }),
            ),
            remainingCount > 0 &&
              m(
                '.pf-fuzzy-demo-more',
                {
                  style: {
                    fontSize: '12px',
                    fontStyle: 'italic',
                    color: 'var(--pf-color-text-secondary, #777)',
                    textAlign: 'center',
                    padding: '8px 0',
                  },
                },
                `... and ${remainingCount} more`,
              ),
          )
        : m(EmptyState, {title: 'No matching items'}),
    ),
  ];
}
