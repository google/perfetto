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

import {
  QueryNode,
  nextNodeId,
  NodeType,
  QueryNodeState,
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import m from 'mithril';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {setValidationError} from '../node_issues';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {NodeDetailsMessage} from '../node_styling_widgets';
import {loadNodeDoc} from '../node_doc_loader';
import {PerfettoSqlTypes} from '../../../../trace_processor/perfetto_sql_type';

export interface CounterToIntervalsNodeState extends QueryNodeState {}

export class CounterToIntervalsNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kCounterToIntervals;
  primaryInput?: QueryNode;
  nextNodes: QueryNode[];
  readonly state: CounterToIntervalsNodeState;

  constructor(state: CounterToIntervalsNodeState = {}) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.nextNodes = [];
  }

  onPrevNodesUpdated(): void {
    this.state.onchange?.();
  }

  get sourceCols(): ColumnInfo[] {
    return this.primaryInput?.finalCols ?? [];
  }

  getTitle(): string {
    return 'Counter to Intervals';
  }

  get finalCols(): ColumnInfo[] {
    // Return empty array if no primary input
    if (!this.primaryInput) {
      return [];
    }

    const cols = [...this.sourceCols];

    // Add the new columns that counter_leading_intervals! produces
    // dur: the duration until the next counter value
    cols.push({
      name: 'dur',
      type: 'DURATION',
      checked: true,
      column: {name: 'dur', type: PerfettoSqlTypes.DURATION},
    });

    // next_value: the value of the next counter
    cols.push({
      name: 'next_value',
      type: 'DOUBLE',
      checked: true,
      column: {name: 'next_value', type: PerfettoSqlTypes.DOUBLE},
    });

    // delta_value: the change in value (next_value - value)
    cols.push({
      name: 'delta_value',
      type: 'DOUBLE',
      checked: true,
      column: {name: 'delta_value', type: PerfettoSqlTypes.DOUBLE},
    });

    return cols;
  }

  private hasRequiredColumns(): boolean {
    const colNames = new Set(this.sourceCols.map((c) => c.name));
    return (
      colNames.has('id') &&
      colNames.has('ts') &&
      colNames.has('track_id') &&
      colNames.has('value')
    );
  }

  private hasDurColumn(): boolean {
    return this.sourceCols.some((c) => c.name === 'dur');
  }

  validate(): boolean {
    // Clear any previous errors
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.primaryInput === undefined) {
      setValidationError(this.state, 'No input node connected');
      return false;
    }

    if (!this.primaryInput.validate()) {
      setValidationError(this.state, 'Previous node is invalid');
      return false;
    }

    // Verify that source has at least one row of data (not empty)
    if (this.sourceCols.length === 0) {
      setValidationError(this.state, 'Input has no columns');
      return false;
    }

    // Check that input has required columns for counter data
    if (!this.hasRequiredColumns()) {
      setValidationError(
        this.state,
        'Input must have id, ts, track_id, and value columns',
      );
      return false;
    }

    // Check that input does NOT already have dur (it's counter data, not interval data)
    if (this.hasDurColumn()) {
      setValidationError(
        this.state,
        'Input already has dur column (already interval data)',
      );
      return false;
    }

    return true;
  }

  nodeDetails(): NodeDetailsAttrs {
    return {
      content: NodeDetailsMessage('Converts counter data to intervals'),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    return {
      info: 'Converts counter-style data (with ts but no dur) to interval-style data (with ts and dur). The output includes dur (time until the next different counter value), next_value, and delta_value columns.',
    };
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('counter_to_intervals');
  }

  clone(): QueryNode {
    const stateCopy: CounterToIntervalsNodeState = {
      onchange: this.state.onchange,
    };
    return new CounterToIntervalsNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return undefined;
    if (this.primaryInput === undefined) return undefined;

    return StructuredQueryBuilder.withCounterIntervals(
      this.primaryInput,
      this.nodeId,
    );
  }

  serializeState(): object {
    return {
      primaryInputId: this.primaryInput?.nodeId,
    };
  }

  static deserializeState(
    _serializedState: CounterToIntervalsNodeState,
  ): CounterToIntervalsNodeState {
    return {};
  }
}
