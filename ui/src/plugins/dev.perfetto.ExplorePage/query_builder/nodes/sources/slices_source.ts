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
import {
  QueryNode,
  QueryNodeState,
  NodeType,
  createFinalColumns,
  SourceNode,
  nextNodeId,
} from '../../../query_node';
import {ColumnInfo, columnInfoFromSqlColumn} from '../../column_info';
import protos from '../../../../../protos';
import {SqlColumn} from '../../../../dev.perfetto.SqlModules/sql_modules';
import {StructuredQueryBuilder} from '../../structured_query_builder';

export interface SlicesSourceSerializedState {
  comment?: string;
}

export interface SlicesSourceState extends QueryNodeState {
  onchange?: () => void;
}

export class SlicesSourceNode implements SourceNode {
  readonly nodeId: string;
  readonly state: SlicesSourceState;
  readonly finalCols: ColumnInfo[];
  nextNodes: QueryNode[];

  constructor(attrs: SlicesSourceState) {
    this.nodeId = nextNodeId();
    this.state = attrs;
    this.state.onchange = attrs.onchange;
    this.finalCols = createFinalColumns(slicesSourceNodeColumns(true));
    this.nextNodes = [];
  }

  get type() {
    return NodeType.kSimpleSlices;
  }

  validate(): boolean {
    return true;
  }

  clone(): QueryNode {
    const stateCopy: SlicesSourceState = {
      onchange: this.state.onchange,
    };
    return new SlicesSourceNode(stateCopy);
  }

  getTitle(): string {
    return 'Slices with details';
  }

  serializeState(): SlicesSourceSerializedState {
    return {
      comment: this.state.comment,
    };
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const sq = StructuredQueryBuilder.fromTable(
      'thread_or_process_slice',
      'slices.with_context',
      undefined,
      this.nodeId,
    );

    // Manually create selectColumns for the specific columns we want
    const selectColumns: protos.PerfettoSqlStructuredQuery.SelectColumn[] = [];
    for (const col of this.finalCols) {
      const selectColumn = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectColumn.columnName = col.column.name;
      selectColumns.push(selectColumn);
    }
    sq.selectColumns = selectColumns;

    return sq;
  }

  nodeSpecificModify(): m.Child {
    return undefined;
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Provides slice data from your trace. Slices represent time intervals with start time (',
        m('code', 'ts'),
        ') and duration (',
        m('code', 'dur'),
        '), tracking spans of execution like function calls, scheduling periods, or GPU work.',
      ),
      m(
        'p',
        'Includes context like process and thread information, making it easy to analyze execution patterns.',
      ),
    );
  }
}

export function slicesSourceNodeColumns(checked: boolean): ColumnInfo[] {
  const cols: SqlColumn[] = [
    {
      name: 'id',
      type: {
        kind: 'id',
        source: {
          table: 'slice',
          column: 'id',
        },
      },
    },
    {
      name: 'ts',
      type: {
        kind: 'timestamp',
      },
    },
    {
      name: 'dur',
      type: {
        kind: 'duration',
      },
    },
    {
      name: 'name',
      type: {
        kind: 'string',
      },
    },
    {
      name: 'track_id',
      type: {
        kind: 'joinid',
        source: {
          table: 'track',
          column: 'id',
        },
      },
    },
    {
      name: 'process_name',
      type: {
        kind: 'string',
      },
    },
    {
      name: 'upid',
      type: {
        kind: 'joinid',
        source: {
          table: 'process',
          column: 'id',
        },
      },
    },
    {
      name: 'thread_name',
      type: {
        kind: 'string',
      },
    },
    {
      name: 'utid',
      type: {
        kind: 'joinid',
        source: {
          table: 'thread',
          column: 'id',
        },
      },
    },
    {
      name: 'depth',
      type: {
        kind: 'int',
      },
    },
    {
      name: 'parent_id',
      type: {
        kind: 'joinid',
        source: {
          table: 'slice',
          column: 'id',
        },
      },
    },
    {
      name: 'category',
      type: {
        kind: 'string',
      },
    },
  ];
  return cols.map((c) => columnInfoFromSqlColumn(c, checked));
}
