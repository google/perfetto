// Copyright (C) 2023 The Android Open Source Project
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
  ColumnDescriptor,
  numberColumn,
  stringColumn,
  Table,
  TableData,
} from './table';

// This file serves as an example of a table component present in the widgets
// showcase. Since table is somewhat complicated component that requires some
// setup spread across several declarations, all the necessary code resides in a
// separate file (this one) and provides a no-argument wrapper component that
// can be used in the widgets showcase directly.

interface ProgrammingLanguage {
  id: number;
  name: string;
  year: number;
}

const languagesList: ProgrammingLanguage[] = [
  {
    id: 1,
    name: 'TypeScript',
    year: 2012,
  },
  {
    id: 2,
    name: 'JavaScript',
    year: 1995,
  },
  {
    id: 3,
    name: 'Lean',
    year: 2013,
  },
];

const columns: ColumnDescriptor<ProgrammingLanguage>[] = [
  numberColumn('ID', (x) => x.id),
  stringColumn('Name', (x) => x.name),
  numberColumn('Year', (x) => x.year),
];

export class TableShowcase implements m.ClassComponent {
  data = new TableData(languagesList);

  view(): m.Child {
    return m(Table, {
      data: this.data,
      columns,
    });
  }
}
