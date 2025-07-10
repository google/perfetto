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
  ColumnInfo,
  columnInfoFromSqlColumn,
  newColumnInfoList,
} from '../column_info';
import protos from '../../../../protos';
import {TextInput} from '../../../../widgets/text_input';
import {SqlColumn} from '../../../dev.perfetto.SqlModules/sql_modules';
import {TableAndColumnImpl} from '../../../dev.perfetto.SqlModules/sql_modules_impl';
import {
  createFiltersProto,
  createGroupByProto,
} from '../operations/operation_component';

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

  readonly sourceCols: ColumnInfo[];
  readonly finalCols: ColumnInfo[];

  readonly state: SlicesSourceAttrs;

  constructor(attrs: SlicesSourceAttrs) {
    this.state = attrs;
    this.sourceCols = slicesSourceNodeColumns(true);
    this.finalCols = createFinalColumns(this);
  }

  getStateCopy(): QueryNodeState {
    const newState: SlicesSourceAttrs = {
      slice_name: this.state.slice_name?.slice(),
      thread_name: this.state.thread_name?.slice(),
      process_name: this.state.process_name?.slice(),
      track_name: this.state.track_name?.slice(),
      sourceCols: newColumnInfoList(this.sourceCols),
      groupByColumns: newColumnInfoList(this.state.groupByColumns),
      filters: this.state.filters.map((f) => ({...f})),
      aggregations: this.state.aggregations.map((a) => ({...a})),
      customTitle: this.state.customTitle,
    };
    return newState;
  }

  validate(): boolean {
    return true;
  }

  getTitle(): string {
    return this.state.customTitle ?? 'Simple slices';
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

  coreModify(): m.Child {
    return m(
      '',
      m(
        '.pf-slice-source-box',
        m(
          'div',
          {
            class: 'pf-slice-source-label',
          },
          m('span', 'Slice name'),
          m(TextInput, {
            id: 'slice_name_glob',
            type: 'string',
            placeholder: 'MySlice*',
            oninput: (e: Event) => {
              if (!e.target) return;
              this.state.slice_name = (
                e.target as HTMLInputElement
              ).value.trim();
            },
          }),
        ),
        m(
          'div',
          {
            class: 'pf-slice-source-label',
          },
          m('span', 'Thread name'),
          m(TextInput, {
            id: 'thread_name_glob',
            type: 'string',
            placeholder: 'RenderThread',
            oninput: (e: Event) => {
              if (!e.target) return;
              this.state.thread_name = (
                e.target as HTMLInputElement
              ).value.trim();
            },
          }),
        ),
        m(
          'div',
          {
            class: 'pf-slice-source-label',
          },
          m('span', 'Process name'),
          m(TextInput, {
            id: 'process_name_glob',
            type: 'string',
            placeholder: '*chrome*',
            oninput: (e: Event) => {
              if (!e.target) return;
              this.state.process_name = (
                e.target as HTMLInputElement
              ).value.trim();
            },
          }),
        ),
        m(
          'div',
          {
            class: 'pf-slice-source-label',
          },
          m('span', 'Track name'),
          m(TextInput, {
            id: 'track_name_glob',
            type: 'string',
            placeholder: 'SurfaceFlinger',
            oninput: (e: Event) => {
              if (!e.target) return;
              this.state.track_name = (
                e.target as HTMLInputElement
              ).value.trim();
            },
          }),
        ),
      ),
    );
  }
}

export function slicesSourceNodeColumns(checked: boolean): ColumnInfo[] {
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
  return cols.map((c) => columnInfoFromSqlColumn(c, checked));
}
