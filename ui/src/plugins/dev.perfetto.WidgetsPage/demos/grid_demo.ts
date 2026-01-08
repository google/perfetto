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
import {Grid, GridCell, GridHeaderCell, GridRow} from '../../../widgets/grid';
import {MenuItem} from '../../../widgets/menu';
import {renderWidgetShowcase} from '../widgets_page_utils';
import {languages} from '../sample_data';
import {Anchor} from '../../../widgets/anchor';
import {CodeSnippet} from '../../../widgets/code_snippet';

export function renderGrid(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Grid'),
      m('p', [
        'Grid is a ',
        m('code', '<table>'),
        ' on steroids! It adds quality of life features such as:',
      ]),
      m('ul', [
        m('li', 'Virtualization'),
        m('li', 'Column resizing with auto sizing functionality'),
        m('li', 'Cell and header level context menus'),
        m('li', 'Indentation + chevrons for representing tree-like data grids'),
      ]),
      m('p', [
        'Grid is utterly unopinionated about the data it displays, ',
        'and does not impose any structure on how that data should look ',
        'and where it should be loaded from. ',
        'Because of this, it can be used as-is to simply throw some data ',
        'up on the screen, or it can be used as a foundational building block ',
        'for more complex widgets. See ',
        m(Anchor, {href: '#!/widgets/datagrid'}, 'DataGrid'),
        '.',
      ]),
    ),

    m('h2', 'Interactive Demo'),

    renderWidgetShowcase({
      renderWidget: ({
        virtualize,
        wrap,
        treeDemo,
        contextMenus,
        sortArrows,
      }) => {
        if (virtualize) {
          return m(VirtualGridDemo, {
            contextMenus,
            sortArrows,
          });
        } else {
          return renderSimpleGridDemo(wrap, treeDemo, contextMenus, sortArrows);
        }
      },
      initialOpts: {
        contextMenus: false,
        sortArrows: false,
        virtualize: false,
        wrap: false,
        treeDemo: false,
      },
      noPadding: true,
    }),

    m('h2', 'Basic Usage'),
    m('p', [
      'At its simplest, Grid requires just ',
      m('code', 'columns'),
      ' and ',
      m('code', 'rowData'),
      '. Each row is an array of GridCell elements. ',
      'Columns and rows can contain absolutely any mithril vnodes at all, ',
      'and still take advantage of column sizing and virtualization, ',
      'but the pre-packaged GridHeaderCell and GridCell components provide ',
      'bootstrapped cells with some useful functionality such as context menus, ',
      'sorting, and indentation.',
    ]),
    m(
      'p',
      m(CodeSnippet, {
        text: `m(Grid, {
  columns: [
    {key: 'name', header: m(GridHeaderCell, 'Name')},
    {key: 'value', header: m(GridHeaderCell, 'Value')},
  ],
  rowData: [
    [m(GridCell, 'Row 1'), m(GridCell, 'Data 1')],
    [m(GridCell, 'Row 2'), m(GridCell, 'Data 2')],
    [m(GridCell, 'Row 3'), m(GridCell, 'Data 3')],
  ],
  fillHeight: true,
});`,
      }),
    ),
  ];
}

interface TreeNode {
  readonly id: string;
  readonly type: 'decade' | 'typing' | 'language';
  readonly decade?: string;
  readonly typing?: string;
  readonly langData?: (typeof languages)[number];
}

function HierarchicalGridDemo(): m.ClassComponent<{
  readonly contextMenus: boolean;
  readonly sortArrows: boolean;
}> {
  const expandedNodes = new Set<string>(['1970s', '1980s']);

  // Build tree structure
  const buildTree = (): TreeNode[] => {
    const tree: TreeNode[] = [];
    type LanguagesByTyping = Map<string, (typeof languages)[number][]>;
    const byDecade = new Map<string, LanguagesByTyping>();

    // Group languages by decade and typing
    languages.forEach((lang) => {
      const decade = `${Math.floor(lang.year / 10) * 10}s`;
      if (!byDecade.has(decade)) {
        byDecade.set(decade, new Map());
      }
      const decadeMap = byDecade.get(decade)!;
      if (!decadeMap.has(lang.typing)) {
        decadeMap.set(lang.typing, []);
      }
      decadeMap.get(lang.typing)!.push(lang);
    });

    // Convert to tree nodes
    const sortedDecades = Array.from(byDecade.keys()).sort();
    sortedDecades.forEach((decade) => {
      tree.push({id: decade, type: 'decade', decade});

      if (expandedNodes.has(decade)) {
        const typingMap = byDecade.get(decade)!;
        const typings = Array.from(typingMap.keys()).sort();

        typings.forEach((typing) => {
          const typingId = `${decade}-${typing}`;
          tree.push({id: typingId, type: 'typing', decade, typing});

          if (expandedNodes.has(typingId)) {
            const langs = typingMap.get(typing)!;
            langs.forEach((lang) => {
              tree.push({
                id: `${typingId}-${lang.lang}`,
                type: 'language',
                decade,
                typing,
                langData: lang,
              });
            });
          }
        });
      }
    });

    return tree;
  };

  const toggleNode = (nodeId: string) => {
    if (expandedNodes.has(nodeId)) {
      expandedNodes.delete(nodeId);
    } else {
      expandedNodes.add(nodeId);
    }
  };

  return {
    view: ({attrs}) => {
      const {contextMenus, sortArrows} = attrs;
      const tree = buildTree();

      const makeHeaderMenuItems = (columnKey: string) => {
        if (!contextMenus) return undefined;
        return [
          m(MenuItem, {
            label: `Hide "${columnKey}" column`,
            onclick: () => alert(`Menu: Hide ${columnKey} column`),
          }),
          m(MenuItem, {
            label: 'Auto-fit width',
            onclick: () => alert(`Menu: Auto-fit ${columnKey}`),
          }),
        ];
      };

      return m(Grid, {
        key: 'hierarchical-grid-demo',
        columns: [
          {
            key: 'name',
            header: m(
              GridHeaderCell,
              {
                sort: sortArrows ? 'ASC' : undefined,
                onSort: sortArrows ? () => alert('Sort clicked') : undefined,
                menuItems: makeHeaderMenuItems('name'),
              },
              'Name',
            ),
          },
          {
            key: 'year',
            header: m(
              GridHeaderCell,
              {
                sort: sortArrows ? 'DESC' : undefined,
                onSort: sortArrows ? () => alert('Sort clicked') : undefined,
                menuItems: makeHeaderMenuItems('year'),
              },
              'Year',
            ),
          },
          {
            key: 'creator',
            header: m(
              GridHeaderCell,
              {
                menuItems: makeHeaderMenuItems('creator'),
              },
              'Creator',
            ),
          },
        ],
        rowData: tree.map((node) => {
          const isExpanded = expandedNodes.has(node.id);

          // Determine indent, chevron, and lastChild based on node type
          let indent: number | undefined;
          let chevron: 'expanded' | 'collapsed' | 'leaf' | undefined;

          if (node.type === 'decade') {
            indent = 0;
            chevron = isExpanded ? 'expanded' : 'collapsed';
          } else if (node.type === 'typing') {
            indent = 1;
            chevron = isExpanded ? 'expanded' : 'collapsed';
            // Check if this is the last typing in its decade
          } else {
            // language
            indent = 2;
            chevron = undefined;
            // Check if this is the last language in its typing group
          }

          const menuItems = contextMenus
            ? [
                m(MenuItem, {
                  label: `Copy "${node.id}"`,
                  onclick: () => navigator.clipboard.writeText(node.id),
                }),
                m(MenuItem, {
                  label: 'Show details',
                  onclick: () => alert(`Node: ${node.id}`),
                }),
              ]
            : undefined;

          if (node.type === 'decade') {
            return [
              m(
                GridCell,
                {
                  indent,
                  chevron,
                  onChevronClick: () => toggleNode(node.id),
                  menuItems,
                },
                node.decade,
              ),
              m(GridCell, {menuItems}, ''),
              m(GridCell, {menuItems}, ''),
            ];
          } else if (node.type === 'typing') {
            return [
              m(
                GridCell,
                {
                  indent,
                  chevron,
                  onChevronClick: () => toggleNode(node.id),
                  menuItems,
                },
                node.typing,
              ),
              m(GridCell, {menuItems}, ''),
              m(GridCell, {menuItems}, ''),
            ];
          } else {
            // language
            const lang = node.langData!;
            return [
              m(
                GridCell,
                {
                  indent,
                  chevron,
                  menuItems,
                },
                lang.lang,
              ),
              m(GridCell, {align: 'right', menuItems}, lang.year),
              m(GridCell, {menuItems}, lang.creator),
            ];
          }
        }),
        fillHeight: true,
      });
    },
  };
}

function renderSimpleGridDemo(
  wrap: boolean,
  treeDemo: boolean,
  contextMenus: boolean,
  sortArrows: boolean,
) {
  // If tree demo is enabled, show the hierarchical tree demo
  if (treeDemo) {
    return m(HierarchicalGridDemo, {contextMenus, sortArrows});
  }

  // Otherwise show the simple flat demo
  const makeHeaderMenuItems = (columnKey: string) => {
    if (!contextMenus) return undefined;
    return [
      m(MenuItem, {
        label: `Hide "${columnKey}" column`,
        onclick: () => alert(`Menu: Hide ${columnKey} column`),
      }),
      m(MenuItem, {
        label: 'Auto-fit width',
        onclick: () => alert(`Menu: Auto-fit ${columnKey}`),
      }),
    ];
  };

  // Generate bogus data with various edge cases
  const bogusData = [
    {
      id: 1,
      name: 'Alice',
      score: 95,
      notes: 'Excellent performance in all areas',
      status: 'active',
    },
    {id: 2, name: 'Bob', score: null, notes: null, status: 'pending'},
    {
      id: 3,
      name: 'Charlie',
      score: 78,
      notes: 'Needs improvement',
      status: 'active',
    },
    {id: 4, name: null, score: 88, notes: 'Anonymous entry', status: null},
    {
      id: 5,
      name: 'Diana',
      score: 100,
      notes:
        'This is an extremely long note that should test the max initial width clamping behavior of the grid column sizing logic. It just keeps going and going and going, with more and more text to really push the boundaries of what a reasonable column width should be.',
      status: 'active',
    },
    {id: 6, name: 'Eve', score: undefined, notes: '', status: 'inactive'},
    {id: 7, name: 'Frank', score: 45, notes: 'Below average', status: 'active'},
    {id: 8, name: 'Grace', score: 82, notes: null, status: 'pending'},
    {id: 9, name: 'Henry', score: 91, notes: 'Good work', status: 'active'},
    {
      id: 10,
      name: 'Ivy',
      score: 0,
      notes: 'Zero score (not null)',
      status: 'inactive',
    },
  ];

  return m(Grid, {
    key: 'grid-demo-no-virt',
    columns: [
      {
        key: 'id',
        widthPx: 60, // Fixed width column
        header: m(
          GridHeaderCell,
          {
            sort: sortArrows ? 'ASC' : undefined,
            onSort: sortArrows ? () => alert('Sort clicked') : undefined,
            menuItems: makeHeaderMenuItems('id'),
          },
          'ID',
        ),
      },
      {
        key: 'name',
        minWidthPx: 120, // Min width column
        header: m(
          GridHeaderCell,
          {
            menuItems: makeHeaderMenuItems('name'),
          },
          'Name',
        ),
      },
      {
        key: 'score',
        header: m(
          GridHeaderCell,
          {
            sort: sortArrows ? 'DESC' : undefined,
            onSort: sortArrows ? () => alert('Sort clicked') : undefined,
            menuItems: makeHeaderMenuItems('score'),
          },
          'Score',
        ),
      },
      {
        key: 'notes',
        maxInitialWidthPx: 200, // Max initial width column
        header: m(
          GridHeaderCell,
          {
            menuItems: makeHeaderMenuItems('notes'),
          },
          'Notes',
        ),
      },
      {
        key: 'status',
        header: m(
          GridHeaderCell,
          {
            menuItems: makeHeaderMenuItems('status'),
          },
          'Status',
        ),
      },
    ],
    rowData: bogusData.map((row) => {
      const menuItems = contextMenus
        ? [
            m(MenuItem, {
              label: `Copy row ${row.id}`,
              onclick: () => navigator.clipboard.writeText(JSON.stringify(row)),
            }),
            m(MenuItem, {
              label: 'Show details',
              onclick: () => alert(JSON.stringify(row, null, 2)),
            }),
          ]
        : undefined;

      // Render nullish values with a placeholder style
      const renderValue = (value: unknown) => {
        if (value === null || value === undefined) {
          return m('span.pf-text-muted', '—');
        }
        if (value === '') {
          return m('span.pf-text-muted', '(empty)');
        }
        return String(value);
      };

      return [
        m(GridCell, {wrap, align: 'right', menuItems}, row.id),
        m(GridCell, {wrap, menuItems}, renderValue(row.name)),
        m(GridCell, {wrap, align: 'right', menuItems}, renderValue(row.score)),
        m(GridCell, {wrap, menuItems}, renderValue(row.notes)),
        m(GridCell, {wrap, menuItems}, renderValue(row.status)),
      ];
    }),
    fillHeight: true,
  });
}

interface VirtualGridDemoAttrs {
  readonly contextMenus: boolean;
  readonly sortArrows: boolean;
}

function VirtualGridDemo(): m.ClassComponent<VirtualGridDemoAttrs> {
  const totalRows = 10_000;
  let currentOffset = 0;
  let loadedRows: GridRow[] = [];

  const loadData = (offset: number, limit: number, contextMenus: boolean) => {
    currentOffset = offset;
    loadedRows = [];
    for (let i = 0; i < limit && offset + i < totalRows; i++) {
      const idx = offset + i;
      const langData = languages[idx % languages.length];

      const menuItems = contextMenus
        ? [
            m(MenuItem, {
              label: `Copy "${langData.lang}"`,
              onclick: () => navigator.clipboard.writeText(langData.lang),
            }),
            m(MenuItem, {
              label: 'Show row details',
              onclick: () => alert(`Row ${idx + 1}: ${langData.lang}`),
            }),
          ]
        : undefined;

      loadedRows.push([
        m(GridCell, {align: 'right', menuItems}, idx + 1),
        m(GridCell, {menuItems}, langData.lang),
        m(GridCell, {align: 'right', menuItems}, langData.year),
        m(GridCell, {menuItems}, langData.creator),
        m(GridCell, {menuItems}, langData.typing),
      ]);
    }
    m.redraw();
  };

  const makeHeaderMenuItems = (columnKey: string) => {
    return [
      m(MenuItem, {
        label: `Hide "${columnKey}" column`,
        onclick: () => alert(`Menu: Hide ${columnKey} column`),
      }),
      m(MenuItem, {
        label: 'Auto-fit width',
        onclick: () => alert(`Menu: Auto-fit ${columnKey}`),
      }),
    ];
  };

  return {
    view: ({attrs}) => {
      const {contextMenus, sortArrows} = attrs;
      return m(Grid, {
        key: 'virtual-grid',
        columns: [
          {
            key: 'id',
            header: m(
              GridHeaderCell,
              {
                sort: sortArrows ? 'ASC' : undefined,
                onSort: sortArrows ? () => alert('Sort clicked') : undefined,
                menuItems: contextMenus ? makeHeaderMenuItems('id') : undefined,
              },
              'ID',
            ),
          },
          {
            key: 'lang',
            header: m(
              GridHeaderCell,
              {
                menuItems: contextMenus
                  ? makeHeaderMenuItems('lang')
                  : undefined,
              },
              'Language',
            ),
          },
          {
            key: 'year',
            header: m(
              GridHeaderCell,
              {
                sort: sortArrows ? 'DESC' : undefined,
                onSort: sortArrows ? () => alert('Sort clicked') : undefined,
                menuItems: contextMenus
                  ? makeHeaderMenuItems('year')
                  : undefined,
              },
              'Year',
            ),
          },
          {
            key: 'creator',
            header: m(
              GridHeaderCell,
              {
                menuItems: contextMenus
                  ? makeHeaderMenuItems('creator')
                  : undefined,
              },
              'Creator',
            ),
          },
          {
            key: 'typing',
            header: m(
              GridHeaderCell,
              {
                menuItems: contextMenus
                  ? makeHeaderMenuItems('typing')
                  : undefined,
              },
              'Typing',
            ),
          },
        ],
        rowData: {
          data: loadedRows,
          total: totalRows,
          offset: currentOffset,
          onLoadData: (offset, limit) => loadData(offset, limit, contextMenus),
        },
        virtualization: {
          rowHeightPx: 24,
        },
        fillHeight: true,
      });
    },
  };
}
