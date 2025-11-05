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
  createSelectColumnsProto,
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
import {
  createExperimentalFiltersProto,
  renderFilterOperation,
  UIFilter,
} from '../../operations/filter';

export interface SlicesSourceSerializedState {
  filters?: UIFilter[];
  filterOperator?: 'AND' | 'OR';
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
    this.state.filters = attrs.filters ?? [];
  }

  get type() {
    return NodeType.kSimpleSlices;
  }

  validate(): boolean {
    return true;
  }

  clone(): QueryNode {
    const stateCopy: SlicesSourceState = {
      filters: this.state.filters?.map((f) => ({...f})),
      onchange: this.state.onchange,
    };
    return new SlicesSourceNode(stateCopy);
  }

  getTitle(): string {
    return 'Slices with details';
  }

  serializeState(): SlicesSourceSerializedState {
    return {
      filters: this.state.filters,
      filterOperator: this.state.filterOperator,
      comment: this.state.comment,
    };
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = this.nodeId;
    sq.table = new protos.PerfettoSqlStructuredQuery.Table();
    sq.table.tableName = 'thread_or_process_slice';
    sq.table.moduleName = 'slices.with_context';
    sq.table.columnNames = this.finalCols
      .filter((c) => c.checked)
      .map((c) => c.column.name);

    const filtersProto = createExperimentalFiltersProto(
      this.state.filters,
      this.finalCols,
      this.state.filterOperator,
    );
    if (filtersProto) sq.experimentalFilterGroup = filtersProto;

    const selectedColumns = createSelectColumnsProto(this);
    if (selectedColumns) sq.selectColumns = selectedColumns;

    return sq;
  }

  nodeSpecificModify(): m.Child {
    return renderFilterOperation(
      this.state.filters,
      this.state.filterOperator,
      this.finalCols,
      (newFilters) => {
        this.state.filters = [...newFilters];
        this.state.onchange?.();
      },
      (operator) => {
        this.state.filterOperator = operator;
        this.state.onchange?.();
      },
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
  ];
  return cols.map((c) => columnInfoFromSqlColumn(c, checked));
}
