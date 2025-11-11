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
  nextNodeId,
  NodeType,
  ModificationNode,
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {
  UIFilter,
  FilterGroup,
  renderFilterOperation,
  createExperimentalFiltersProto,
  formatFilterDetails,
} from '../operations/filter';
import {StructuredQueryBuilder} from '../structured_query_builder';

export interface FilterNodeState extends QueryNodeState {
  prevNode: QueryNode;
  filters?: UIFilter[];
  filterOperator?: 'AND' | 'OR';
  groups?: FilterGroup[]; // OR groups that are ANDed together at top level
}

export class FilterNode implements ModificationNode {
  readonly nodeId: string;
  readonly type = NodeType.kFilter;
  readonly prevNode: QueryNode;
  nextNodes: QueryNode[];
  readonly state: FilterNodeState;

  constructor(state: FilterNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.prevNode = state.prevNode;
    this.nextNodes = [];
  }

  get sourceCols(): ColumnInfo[] {
    return this.prevNode?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    return this.sourceCols;
  }

  getTitle(): string {
    return 'Filter';
  }

  nodeDetails(): m.Child {
    const hasFilters = this.state.filters && this.state.filters.length > 0;
    const hasGroups = this.state.groups && this.state.groups.length > 0;

    if (!hasFilters && !hasGroups) {
      return m('.pf-filter-node-details', 'No filters applied');
    }

    return formatFilterDetails(
      this.state.filters,
      this.state.filterOperator,
      this.state.groups,
      this.state, // Pass state for interactive toggling and removal
      undefined, // onRemove - handled internally by formatFilterDetails
      true, // compact mode for smaller font
    );
  }

  nodeSpecificModify(): m.Child {
    return renderFilterOperation(
      this.state.filters,
      this.state.filterOperator,
      this.sourceCols,
      (newFilters) => {
        this.state.filters = [...newFilters];
        this.state.onchange?.();
      },
      (operator) => {
        this.state.filterOperator = operator;
        this.state.onchange?.();
      },
      this.state.groups,
      (newGroups) => {
        this.state.groups = [...newGroups];
        this.state.onchange?.();
      },
    );
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Keep only rows that match conditions you specify. Supports operators like ',
        m('code', '='),
        ', ',
        m('code', '>'),
        ', ',
        m('code', '<'),
        ', ',
        m('code', 'glob'),
        ', and null checks.',
      ),
      m(
        'p',
        'Combine multiple conditions with ',
        m('code', 'AND'),
        ' or ',
        m('code', 'OR'),
        ' logic. Drag filters onto each other to create OR groups.',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Keep slices where ',
        m('code', 'dur > 1000000'),
        ' AND ',
        m('code', 'name glob "*render*"'),
      ),
    );
  }

  validate(): boolean {
    return this.prevNode !== undefined;
  }

  clone(): QueryNode {
    return new FilterNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.prevNode === undefined) return undefined;

    const hasFilters = this.state.filters && this.state.filters.length > 0;
    const hasGroups = this.state.groups && this.state.groups.length > 0;

    // If no filters and no groups are defined, just return the previous node's query
    if (!hasFilters && !hasGroups) {
      return this.prevNode.getStructuredQuery();
    }

    const filtersProto = createExperimentalFiltersProto(
      this.state.filters,
      this.sourceCols,
      this.state.filterOperator,
      this.state.groups,
    );

    if (!filtersProto) {
      return this.prevNode.getStructuredQuery();
    }

    return StructuredQueryBuilder.withFilter(
      this.prevNode,
      filtersProto,
      this.nodeId,
    );
  }

  serializeState(): object {
    return {
      filters: this.state.filters?.map((f) => {
        if ('value' in f) {
          return {
            column: f.column,
            op: f.op,
            value: f.value,
            enabled: f.enabled,
          };
        } else {
          return {
            column: f.column,
            op: f.op,
            enabled: f.enabled,
          };
        }
      }),
      filterOperator: this.state.filterOperator,
      groups: this.state.groups?.map((g) => ({
        id: g.id,
        enabled: g.enabled,
        filters: g.filters.map((f) => {
          if ('value' in f) {
            return {
              column: f.column,
              op: f.op,
              value: f.value,
              enabled: f.enabled,
            };
          } else {
            return {
              column: f.column,
              op: f.op,
              enabled: f.enabled,
            };
          }
        }),
      })),
      comment: this.state.comment,
    };
  }

  /**
   * Deserializes a FilterNodeState from JSON.
   *
   * IMPORTANT: This method returns a state with prevNode set to undefined.
   * The caller (typically json_handler.ts) is responsible for:
   * 1. Creating all nodes first with undefined prevNode references
   * 2. Reconnecting the graph by setting prevNode references based on serialized node IDs
   * 3. Calling validate() on each node after reconnection to ensure graph integrity
   *
   * @param state The serialized state (prevNode will be ignored)
   * @returns A FilterNodeState with prevNode set to undefined (to be set by caller)
   */
  static deserializeState(state: FilterNodeState): FilterNodeState {
    return {
      ...state,
      prevNode: undefined as unknown as QueryNode,
    };
  }
}
