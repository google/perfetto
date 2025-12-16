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
import {TreeTable, TreeTableAttrs} from '../../../components/widgets/treetable';
import {renderWidgetShowcase} from '../widgets_page_utils';

interface File {
  name: string;
  size: string;
  date: string;
  children?: File[];
}

const files: File[] = [
  {
    name: 'foo',
    size: '10MB',
    date: '2023-04-02',
  },
  {
    name: 'bar',
    size: '123KB',
    date: '2023-04-08',
    children: [
      {
        name: 'baz',
        size: '4KB',
        date: '2023-05-07',
      },
      {
        name: 'qux',
        size: '18KB',
        date: '2023-05-28',
        children: [
          {
            name: 'quux',
            size: '4KB',
            date: '2023-05-07',
          },
          {
            name: 'corge',
            size: '18KB',
            date: '2023-05-28',
            children: [
              {
                name: 'grault',
                size: '4KB',
                date: '2023-05-07',
              },
              {
                name: 'garply',
                size: '18KB',
                date: '2023-05-28',
              },
              {
                name: 'waldo',
                size: '87KB',
                date: '2023-05-02',
              },
            ],
          },
        ],
      },
    ],
  },
  {
    name: 'fred',
    size: '8KB',
    date: '2022-12-27',
  },
];

export function renderTreeTable(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'TreeTable'),
      m(
        'p',
        'A table component with hierarchical tree structure, combining the features of a tree view with tabular data display.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: () => {
        const attrs: TreeTableAttrs<File> = {
          rows: files,
          getChildren: (file) => file.children,
          columns: [
            {name: 'Name', getData: (file) => file.name},
            {name: 'Size', getData: (file) => file.size},
            {name: 'Date', getData: (file) => file.date},
          ],
        };
        return m(TreeTable<File>, attrs);
      },
      initialOpts: {},
    }),
  ];
}
