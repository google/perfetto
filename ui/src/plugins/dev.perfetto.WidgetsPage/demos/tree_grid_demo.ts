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
import {TreeGrid, TreeGridRow} from '../../../widgets/tree_grid';
import {GridCell, GridHeaderCell} from '../../../widgets/grid';
import {renderWidgetShowcase} from '../widgets_page_utils';
import {Anchor} from '../../../widgets/anchor';
import {CodeSnippet} from '../../../widgets/code_snippet';

export function renderTreeGrid(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'TreeGrid'),
      m('p', [
        'TreeGrid is a specialized version of ',
        m(Anchor, {href: '#!/widgets/grid'}, 'Grid'),
        ' that automatically organizes rows into a tree structure based on slash-separated path keys.',
      ]),
      m('p', [
        'Unlike the lower-level Grid component, TreeGrid handles the tree structure automatically. ',
        'You simply provide flat rows with hierarchical paths, and TreeGrid builds the tree, ',
        'manages expand/collapse state, and renders the appropriate indent levels and chevrons.',
      ]),
      m('ul', [
        m('li', 'Automatic tree building from slash-separated paths'),
        m('li', 'Built-in expand/collapse functionality'),
        m('li', 'Automatic indent and chevron management'),
        m('li', 'Supports virtualization for large datasets'),
        m('li', 'All Grid features (column resizing, sorting, etc.)'),
      ]),
    ),

    m('h2', 'Interactive Demo'),

    renderWidgetShowcase({
      renderWidget: ({virtualize}) => {
        const rows: TreeGridRow[] = [
          {
            path: 'src/base/logging.cc',
            cells: [
              m(GridCell, 'logging.cc'),
              m(GridCell, {align: 'right'}, '1,234'),
              m(GridCell, 'C++'),
            ],
          },
          {
            path: 'src/base/string_utils.cc',
            cells: [
              m(GridCell, 'string_utils.cc'),
              m(GridCell, {align: 'right'}, '856'),
              m(GridCell, 'C++'),
            ],
          },
          {
            path: 'src/base/utils.cc',
            cells: [
              m(GridCell, 'utils.cc'),
              m(GridCell, {align: 'right'}, '432'),
              m(GridCell, 'C++'),
            ],
          },
          {
            path: 'src/trace_processor/db/table.cc',
            cells: [
              m(GridCell, 'table.cc'),
              m(GridCell, {align: 'right'}, '2,145'),
              m(GridCell, 'C++'),
            ],
          },
          {
            path: 'src/trace_processor/db/column.cc',
            cells: [
              m(GridCell, 'column.cc'),
              m(GridCell, {align: 'right'}, '987'),
              m(GridCell, 'C++'),
            ],
          },
          {
            path: 'ui/src/widgets/grid.ts',
            cells: [
              m(GridCell, 'grid.ts'),
              m(GridCell, {align: 'right'}, '1,543'),
              m(GridCell, 'TypeScript'),
            ],
          },
          {
            path: 'ui/src/widgets/tree_grid.ts',
            cells: [
              m(GridCell, 'tree_grid.ts'),
              m(GridCell, {align: 'right'}, '267'),
              m(GridCell, 'TypeScript'),
            ],
          },
          {
            path: 'ui/src/widgets/button.ts',
            cells: [
              m(GridCell, 'button.ts'),
              m(GridCell, {align: 'right'}, '321'),
              m(GridCell, 'TypeScript'),
            ],
          },
          {
            path: 'ui/src/assets/widgets/grid.scss',
            cells: [
              m(GridCell, 'grid.scss'),
              m(GridCell, {align: 'right'}, '341'),
              m(GridCell, 'SCSS'),
            ],
          },
          {
            path: 'ui/src/assets/theme.scss',
            cells: [
              m(GridCell, 'theme.scss'),
              m(GridCell, {align: 'right'}, '156'),
              m(GridCell, 'SCSS'),
            ],
          },
          {
            path: 'docs/README.md',
            cells: [
              m(GridCell, 'README.md'),
              m(GridCell, {align: 'right'}, '89'),
              m(GridCell, 'Markdown'),
            ],
          },
          {
            path: 'docs/contributing.md',
            cells: [
              m(GridCell, 'contributing.md'),
              m(GridCell, {align: 'right'}, '432'),
              m(GridCell, 'Markdown'),
            ],
          },
        ];

        return m(TreeGrid, {
          key: virtualize ? 'treegrid-virtualized' : 'treegrid-full',
          columns: [
            {
              key: 'name',
              header: m(GridHeaderCell, 'File'),
            },
            {
              key: 'lines',
              header: m(GridHeaderCell, 'Lines'),
            },
            {
              key: 'language',
              header: m(GridHeaderCell, 'Language'),
            },
          ],
          rows,
          fillHeight: true,
          virtualization: virtualize
            ? {
                rowHeightPx: 24,
              }
            : undefined,
        });
      },
      initialOpts: {
        virtualize: false,
      },
      noPadding: true,
    }),

    m('h2', 'Basic Usage'),
    m('p', [
      'TreeGrid automatically builds a tree from flat rows with slash-separated paths. ',
      'Each row must have a ',
      m('code', 'path'),
      ' and an array of ',
      m('code', 'cells'),
      '.',
    ]),
    m(
      'p',
      m(CodeSnippet, {
        text: `m(TreeGrid, {
  columns: [
    {key: 'name', header: m(GridHeaderCell, 'Name')},
    {key: 'size', header: m(GridHeaderCell, 'Size')},
  ],
  rows: [
    {
      path: 'root/folder1/file1.txt',
      cells: [m(GridCell, 'file1.txt'), m(GridCell, '1.2 KB')],
    },
    {
      path: 'root/folder1/file2.txt',
      cells: [m(GridCell, 'file2.txt'), m(GridCell, '856 B')],
    },
    {
      path: 'root/folder2/file3.txt',
      cells: [m(GridCell, 'file3.txt'), m(GridCell, '3.4 KB')],
    },
  ],
  fillHeight: true,
});`,
      }),
    ),

    m('h2', 'How It Works'),
    m('p', [
      'TreeGrid parses the ',
      m('code', 'path'),
      ' field of each row (using ',
      m('code', '/'),
      ' as separator by default) and builds a tree structure. ',
      'Intermediate nodes (folders) are created automatically and can be expanded/collapsed. ',
      'Leaf nodes (files) display the provided cell data.',
    ]),
    m('p', [
      'For example, the path ',
      m('code', '"root/folder1/file.txt"'),
      ' creates:',
    ]),
    m('ul', [
      m('li', [m('code', 'root'), ' - intermediate node (folder)']),
      m('li', [m('code', 'folder1'), ' - intermediate node (folder)']),
      m('li', [m('code', 'file.txt'), ' - leaf node with your cell data']),
    ]),

    m('h2', 'Custom Separator'),
    m('p', [
      'You can use a different separator by setting the ',
      m('code', 'separator'),
      ' prop:',
    ]),
    m(
      'p',
      m(CodeSnippet, {
        text: `m(TreeGrid, {
  separator: '.',
  rows: [
    {
      path: 'com.example.app.MainActivity',
      cells: [...],
    },
  ],
  ...
});`,
      }),
    ),
  ];
}
