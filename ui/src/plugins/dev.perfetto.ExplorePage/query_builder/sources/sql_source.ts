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
  columnControllerRowFromName,
} from '../column_controller';
import protos from '../../../../protos';
import {TextParagraph} from '../../../../widgets/text_paragraph';
import {TextInput} from '../../../../widgets/text_input';

export interface SqlSourceAttrs {
  sql?: string;
  columns?: string[];
  preamble?: string;
}

export class SqlSourceNode implements QueryNode {
  type: NodeType = NodeType.kSqlSource;
  prevNode = undefined;
  nextNode?: QueryNode;
  finished = true;

  dataName: string = 'Sql source';
  columns: ColumnControllerRow[];

  attrs: SqlSourceAttrs;

  constructor(attrs: SqlSourceAttrs) {
    this.attrs = attrs;
    this.columns =
      attrs.columns?.map((c) => columnControllerRowFromName(c)) ?? [];
  }

  validate(): boolean {
    return (
      this.attrs.sql !== undefined &&
      this.attrs.columns !== undefined &&
      this.attrs.preamble !== undefined &&
      this.columns.length > 0
    );
  }

  getTitle(): string {
    return `Sql source`;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = `sql_source`;
    const sqlProto = new protos.PerfettoSqlStructuredQuery.Sql();

    if (this.attrs.sql) sqlProto.sql = this.attrs.sql;
    if (this.attrs.columns) sqlProto.columnNames = this.attrs.columns;
    if (this.attrs.preamble) sqlProto.preamble = this.attrs.preamble;

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
    sq.sql = sqlProto;
    sq.selectColumns = selectedColumns;
    return sq;
  }

  getDetails(): m.Child {
    return m(TextParagraph, {
      text: `
        Running custom SQL returning columns ${this.attrs.columns?.join(', ')}.\n
        Preamble: \n${this.attrs.preamble ?? `NONE`}\n
        SQL: \n${this.attrs.sql ?? `NONE`}`,
    });
  }
}

export class SqlSource implements m.ClassComponent<SqlSourceAttrs> {
  view({attrs}: m.CVnode<SqlSourceAttrs>) {
    return m(
      '',
      m(
        '',
        'Preamble',
        m(TextInput, {
          id: 'preamble',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            attrs.preamble = (e.target as HTMLInputElement).value.trim();
          },
        }),
      ),
      m(
        '',
        'Sql ',
        m(TextInput, {
          id: 'sql_source',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            attrs.sql = (e.target as HTMLInputElement).value
              .trim()
              .split(';')[0];
          },
        }),
      ),
      m(
        '',
        'Column names (comma separated strings) ',
        m(TextInput, {
          id: 'columns',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            const colsStr = (e.target as HTMLInputElement).value.trim();
            attrs.columns = colsStr
              .split(',')
              .map((col) => col.trim())
              .filter(Boolean);
          },
        }),
      ),
    );
  }
}
