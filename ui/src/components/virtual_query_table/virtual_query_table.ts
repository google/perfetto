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
import {ColumnType} from '../../trace_processor/query_result';
import {VirtualTable} from '../../widgets/virtual_table';
import {HTMLAttrs} from '../../widgets/common';

interface ColDef {
  readonly name: string;
}

export interface VirtualQueryTableAttrs extends HTMLAttrs {
  // Names and details of the columns.
  readonly columns: ReadonlyArray<ColDef>;

  // Raw row data.
  readonly rows: ReadonlyArray<ReadonlyArray<ColumnType>>;
}

export class VirtualQueryTable
  implements m.ClassComponent<VirtualQueryTableAttrs>
{
  view({attrs}: m.Vnode<VirtualQueryTableAttrs>): m.Children {
    const {columns, rows, ...rest} = attrs;

    return m(VirtualTable, {
      ...rest,
      columns: columns.map((c) => {
        return {
          header: c.name,
          width: '20em',
        };
      }),
      rowHeight: 20,
      firstRowOffset: 0,
      numRows: rows.length,
      rows: rows.map((row, index) => {
        return {
          id: index,
          cells: row.map((cell) => String(cell)),
        };
      }),
    });
  }
}
