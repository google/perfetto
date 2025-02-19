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
import {NodeType, QueryNode} from '../../query_node';
import {
  ColumnControllerRow,
  columnControllerRowFromSqlColumn,
} from '../column_controller';
import protos from '../../../../protos';
import {TextParagraph} from '../../../../widgets/text_paragraph';
import {TextInput} from '../../../../widgets/text_input';
import {SqlColumn} from '../../../dev.perfetto.SqlModules/sql_modules';
import {TableAndColumnImpl} from '../../..//dev.perfetto.SqlModules/sql_modules_impl';

export interface SlicesSourceAttrs {
  slice_name?: string;
  thread_name?: string;
  process_name?: string;
  track_name?: string;
}

export class SlicesSourceNode implements QueryNode {
  type: NodeType = NodeType.kSimpleSlices;
  prevNode = undefined;
  nextNode?: QueryNode;
  finished: boolean = true;

  dataName: string = 'Simple slices';
  columns: ColumnControllerRow[];

  attrs: SlicesSourceAttrs;

  validate(): boolean {
    return (
      this.attrs.slice_name !== undefined ||
      this.attrs.process_name !== undefined ||
      this.attrs.thread_name !== undefined ||
      this.attrs.track_name !== undefined
    );
  }

  getTitle(): string {
    return `Simple slices`;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = `simple_slices_source`;
    const ss = new protos.PerfettoSqlStructuredQuery.SimpleSlices();

    if (this.attrs.slice_name) ss.sliceNameGlob = this.attrs.slice_name;
    if (this.attrs.thread_name) ss.threadNameGlob = this.attrs.thread_name;
    if (this.attrs.process_name) ss.processNameGlob = this.attrs.process_name;
    if (this.attrs.track_name) ss.trackNameGlob = this.attrs.track_name;

    sq.simpleSlices = ss;

    const selectedColumns: protos.PerfettoSqlStructuredQuery.SelectColumn[] =
      [];
    for (const c of this.columns.filter((c) => c.checked)) {
      const newC = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      newC.columnName = c.column.name;
      if (c.alias) {
        newC.alias = c.alias;
      }
      selectedColumns.push(newC);
    }
    sq.selectColumns = selectedColumns;
    return sq;
  }

  constructor(attrs: SlicesSourceAttrs) {
    this.attrs = attrs;
    const cols: SqlColumn[] = [
      {
        name: 'id',
        type: {
          name: 'ID(slice.id)',
          shortName: 'id',
          tableAndColumn: new TableAndColumnImpl('string', 'id'),
        },
      },
      {
        name: 'ts',
        type: {
          name: 'TIMESTAMP',
          shortName: 'TIMESTAMP',
        },
      },
      {
        name: 'dur',
        type: {
          name: 'DURATION',
          shortName: 'DURATION',
        },
      },
      {
        name: 'slice_name',
        type: {
          name: 'STRING',
          shortName: 'STRING',
        },
      },
      {
        name: 'thread_name',
        type: {
          name: 'STRING',
          shortName: 'STRING',
        },
      },
      {
        name: 'process_name',
        type: {
          name: 'STRING',
          shortName: 'STRING',
        },
      },
      {
        name: 'track_name',
        type: {
          name: 'STRING',
          shortName: 'STRING',
        },
      },
    ];
    this.columns = cols.map((c) => columnControllerRowFromSqlColumn(c, true));
  }

  getDetails(): m.Child {
    const s: string[] = [];
    if (this.attrs.slice_name) {
      s.push(`slice name GLOB ${this.attrs.slice_name}`);
    }
    if (this.attrs.thread_name) {
      s.push(`thread name GLOB ${this.attrs.thread_name}`);
    }
    if (this.attrs.process_name) {
      s.push(`process name GLOB ${this.attrs.process_name}`);
    }
    if (this.attrs.track_name) {
      s.push(`track name GLOB ${this.attrs.track_name}`);
    }
    return m(TextParagraph, {text: `Slices where ${s.join(' and ')}`});
  }
}

export class SlicesSource implements m.ClassComponent<SlicesSourceAttrs> {
  view({attrs}: m.CVnode<SlicesSourceAttrs>) {
    return m(
      '',
      m(
        '',
        'Slice name glob ',
        m(TextInput, {
          id: 'slice_name_glob',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            attrs.slice_name = (e.target as HTMLInputElement).value.trim();
          },
        }),
      ),
      m(
        '',
        'Thread name glob ',
        m(TextInput, {
          id: 'thread_name_glob',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            attrs.thread_name = (e.target as HTMLInputElement).value.trim();
          },
        }),
      ),
      m(
        '',
        'Process name glob ',
        m(TextInput, {
          id: 'process_name_glob',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            attrs.process_name = (e.target as HTMLInputElement).value.trim();
          },
        }),
      ),
      m(
        '',
        'Track name glob ',
        m(TextInput, {
          id: 'track_name_glob',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            attrs.track_name = (e.target as HTMLInputElement).value.trim();
          },
        }),
      ),
    );
  }
}
