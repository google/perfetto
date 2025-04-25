// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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
import {CustomTable} from './custom_table';

export interface ColumnDescriptor<T> {
  readonly title: m.Children;
  render: (row: T) => m.Children;
}

export interface BasicTableAttrs<T> {
  readonly data: ReadonlyArray<T>;
  readonly columns: ReadonlyArray<ColumnDescriptor<T>>;
  onreorder?: (from: number, to: number) => void;
  readonly className?: string;
}

export class BasicTable<T> implements m.ClassComponent<BasicTableAttrs<T>> {
  view({attrs}: m.Vnode<BasicTableAttrs<T>>): m.Children {
    return m(CustomTable<T>, {
      columns: [
        {
          columns: attrs.columns.map((c) => ({
            title: c.title,
            render: (row: T) => ({cell: c.render(row)}),
          })),
          reorder: attrs.onreorder,
        },
      ],
      data: attrs.data,
      className: attrs.className,
    });
  }
}
