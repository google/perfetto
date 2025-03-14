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
  createFinalColumns,
  createSelectColumnsProto,
  NodeType,
  QueryNode,
  QueryNodeState,
} from '../../query_node';
import {
  ColumnControllerRow,
  columnControllerRowFromSqlColumn,
  newColumnControllerRows,
} from '../column_controller';
import protos from '../../../../protos';
import {TextParagraph} from '../../../../widgets/text_paragraph';
import {TextInput} from '../../../../widgets/text_input';
import {SqlColumn} from '../../../dev.perfetto.SqlModules/sql_modules';
import {TableAndColumnImpl} from '../../../dev.perfetto.SqlModules/sql_modules_impl';
import {
  createFiltersProto,
  createGroupByProto,
  Operator,
} from '../operations/operation_component';

const slicesCols: ColumnControllerRow[] = [
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
].map((c) => columnControllerRowFromSqlColumn(c, true));

export interface SlicesSourceAttrs extends QueryNodeState {
  slice_name?: string;
  thread_name?: string;
  process_name?: string;
  track_name?: string;
}

export class SlicesSourceNode implements QueryNode {
  type: NodeType = NodeType.kSimpleSlices;
  prevNode = undefined;
  nextNode?: QueryNode;
  readonly finished: boolean = true;

  readonly sourceCols: ColumnControllerRow[];
  readonly finalCols: ColumnControllerRow[];

  readonly state: SlicesSourceAttrs;

  constructor(attrs: SlicesSourceAttrs) {
    this.state = attrs;

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
    this.sourceCols = cols.map((c) =>
      columnControllerRowFromSqlColumn(c, true),
    );

    this.finalCols = createFinalColumns(this);
  }

  getState(): QueryNodeState {
    const newState: SlicesSourceAttrs = {
      slice_name: this.state.slice_name?.slice(),
      thread_name: this.state.thread_name?.slice(),
      process_name: this.state.process_name?.slice(),
      track_name: this.state.track_name?.slice(),
      sourceCols: newColumnControllerRows(this.sourceCols),
      groupByColumns: newColumnControllerRows(this.state.groupByColumns),
      filters: this.state.filters.map((f) => ({...f})),
      aggregations: this.state.aggregations.map((a) => ({...a})),
    };
    return newState;
  }

  validate(): boolean {
    return (
      this.state.slice_name !== undefined ||
      this.state.process_name !== undefined ||
      this.state.thread_name !== undefined ||
      this.state.track_name !== undefined
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

    if (this.state.slice_name) ss.sliceNameGlob = this.state.slice_name;
    if (this.state.thread_name) ss.threadNameGlob = this.state.thread_name;
    if (this.state.process_name) ss.processNameGlob = this.state.process_name;
    if (this.state.track_name) ss.trackNameGlob = this.state.track_name;

    sq.simpleSlices = ss;

    const filtersProto = createFiltersProto(this.state.filters);
    if (filtersProto) sq.filters = filtersProto;
    const groupByProto = createGroupByProto(
      this.state.groupByColumns,
      this.state.aggregations,
    );
    if (groupByProto) sq.groupBy = groupByProto;

    const selectedColumns = createSelectColumnsProto(this);
    if (selectedColumns) sq.selectColumns = selectedColumns;

    return sq;
  }

  getDetails(): m.Child {
    const s: string[] = [];
    if (this.state.slice_name) {
      s.push(`slice name GLOB ${this.state.slice_name}`);
    }
    if (this.state.thread_name) {
      s.push(`thread name GLOB ${this.state.thread_name}`);
    }
    if (this.state.process_name) {
      s.push(`process name GLOB ${this.state.process_name}`);
    }
    if (this.state.track_name) {
      s.push(`track name GLOB ${this.state.track_name}`);
    }
    return m(TextParagraph, {text: `Slices where ${s.join(' and ')}`});
  }
}

export class SlicesSource implements m.ClassComponent<SlicesSourceAttrs> {
  view({attrs}: m.CVnode<SlicesSourceAttrs>) {
    if (attrs.sourceCols.length === 0) {
      attrs.sourceCols = slicesCols;
      attrs.groupByColumns = newColumnControllerRows(slicesCols, false);
    }
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
      m(Operator, {
        filter: {sourceCols: attrs.sourceCols, filters: attrs.filters},
        groupby: {
          groupByColumns: attrs.groupByColumns,
          aggregations: attrs.aggregations,
        },
      }),
    );
  }
}
