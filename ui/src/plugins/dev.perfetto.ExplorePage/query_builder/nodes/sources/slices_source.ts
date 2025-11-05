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
import {Card} from '../../../../../widgets/card';
import {TextInput} from '../../../../../widgets/text_input';
import {SqlColumn} from '../../../../dev.perfetto.SqlModules/sql_modules';
import {
  createExperimentalFiltersProto,
  renderFilterOperation,
  UIFilter,
} from '../../operations/filter';

export interface SlicesSourceSerializedState {
  slice_name?: string;
  thread_name?: string;
  process_name?: string;
  track_name?: string;
  filters?: UIFilter[];
  filterOperator?: 'AND' | 'OR';
  comment?: string;
}

export interface SlicesSourceState extends QueryNodeState {
  slice_name?: string;
  thread_name?: string;
  process_name?: string;
  track_name?: string;
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
      slice_name: this.state.slice_name?.slice(),
      thread_name: this.state.thread_name?.slice(),
      process_name: this.state.process_name?.slice(),
      track_name: this.state.track_name?.slice(),
      filters: this.state.filters ? [...this.state.filters] : undefined,
    };
    return new SlicesSourceNode(stateCopy);
  }

  getTitle(): string {
    return 'Simple slices';
  }

  serializeState(): SlicesSourceSerializedState {
    return {
      slice_name: this.state.slice_name,
      thread_name: this.state.thread_name,
      process_name: this.state.process_name,
      track_name: this.state.track_name,
      filters: this.state.filters,
      filterOperator: this.state.filterOperator,
      comment: this.state.comment,
    };
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = this.nodeId;
    const ss = new protos.PerfettoSqlStructuredQuery.SimpleSlices();

    if (this.state.slice_name) ss.sliceNameGlob = this.state.slice_name;
    if (this.state.thread_name) ss.threadNameGlob = this.state.thread_name;
    if (this.state.process_name) ss.processNameGlob = this.state.process_name;
    if (this.state.track_name) ss.trackNameGlob = this.state.track_name;

    sq.simpleSlices = ss;

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

  nodeDetails?(): m.Child | undefined {
    const details: m.Child[] = [];
    if (this.state.slice_name) {
      details.push(m('div', `slice_name: ${this.state.slice_name}`));
    }
    if (this.state.thread_name) {
      details.push(m('div', `thread_name: ${this.state.thread_name}`));
    }
    if (this.state.process_name) {
      details.push(m('div', `process_name: ${this.state.process_name}`));
    }
    if (this.state.track_name) {
      details.push(m('div', `track_name: ${this.state.track_name}`));
    }

    if (details.length === 0) {
      return;
    }
    return m('.pf-slice-source-details', details);
  }

  nodeSpecificModify(): m.Child {
    return m(
      '',
      m(
        Card,
        m(
          '.pf-slice-source-label',
          m('span', 'Slice name'),
          m(TextInput, {
            id: 'slice_name_glob',
            type: 'string',
            value: this.state.slice_name ?? '',
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
          '.pf-slice-source-label',
          m('span', 'Thread name'),
          m(TextInput, {
            id: 'thread_name_glob',
            type: 'string',
            value: this.state.thread_name ?? '',
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
          '.pf-slice-source-label',
          m('span', 'Process name'),
          m(TextInput, {
            id: 'process_name_glob',
            type: 'string',
            value: this.state.process_name ?? '',
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
          '.pf-slice-source-label',
          m('span', 'Track name'),
          m(TextInput, {
            id: 'track_name_glob',
            type: 'string',
            value: this.state.track_name ?? '',
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
      renderFilterOperation(
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
      name: 'slice_name',
      type: {
        kind: 'string',
      },
    },
    {
      name: 'thread_name',
      type: {
        kind: 'string',
      },
    },
    {
      name: 'process_name',
      type: {
        kind: 'string',
      },
    },
    {
      name: 'track_name',
      type: {
        kind: 'string',
      },
    },
  ];
  return cols.map((c) => columnInfoFromSqlColumn(c, checked));
}
