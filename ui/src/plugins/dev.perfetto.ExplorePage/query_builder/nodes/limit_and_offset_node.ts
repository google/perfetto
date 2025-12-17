// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may a copy of the License at
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
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {setValidationError} from '../node_issues';
import {InlineField} from '../widgets';
import {NodeDetailsAttrs, NodeModifyAttrs} from '../node_explorer_types';
import {createErrorSections} from '../widgets';
import {loadNodeDoc} from '../node_doc_loader';

export interface LimitAndOffsetNodeState extends QueryNodeState {
  limit?: number;
  offset?: number;
}
export class LimitAndOffsetNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kLimitAndOffset;
  primaryInput?: QueryNode;
  nextNodes: QueryNode[];
  readonly state: LimitAndOffsetNodeState;

  constructor(state: LimitAndOffsetNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.nextNodes = [];
    this.state.limit = this.state.limit ?? 10;
    this.state.offset = this.state.offset ?? 0;
  }

  get sourceCols(): ColumnInfo[] {
    return this.primaryInput?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    return this.sourceCols;
  }

  getTitle(): string {
    return 'Limit and Offset';
  }

  nodeDetails(): NodeDetailsAttrs {
    const hasOffset = this.state.offset !== undefined && this.state.offset > 0;
    const limitText = `Limit: ${this.state.limit ?? 10}`;
    const offsetText = hasOffset ? `, Offset: ${this.state.offset}` : '';

    return {
      content: m('div', limitText + offsetText),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    const sections: NodeModifyAttrs['sections'] = [
      ...createErrorSections(this),
    ];

    // Limit and Offset inline fields
    sections.push({
      content: m(
        '.pf-limit-offset-list',
        m(InlineField, {
          label: 'Limit',
          icon: 'filter_list',
          value: this.state.limit?.toString() ?? '10',
          placeholder: 'Number of rows',
          type: 'number',
          validate: (value: string) => {
            const parsed = parseInt(value.trim(), 10);
            return !isNaN(parsed) && parsed >= 0;
          },
          errorMessage: 'Must be a non-negative integer',
          onchange: (value: string) => {
            const parsed = parseInt(value.trim(), 10);
            // Save the parsed value if valid, otherwise keep current value
            this.state.limit =
              !isNaN(parsed) && parsed >= 0 ? parsed : this.state.limit;
            this.state.onchange?.();
          },
        }),
        m(InlineField, {
          label: 'Offset',
          icon: 'skip_next',
          value: this.state.offset?.toString() ?? '0',
          placeholder: 'Number of rows to skip',
          type: 'number',
          validate: (value: string) => {
            const parsed = parseInt(value.trim(), 10);
            return !isNaN(parsed) && parsed >= 0;
          },
          errorMessage: 'Must be a non-negative integer',
          onchange: (value: string) => {
            const parsed = parseInt(value.trim(), 10);
            // Save the parsed value if valid, otherwise keep current value
            this.state.offset =
              !isNaN(parsed) && parsed >= 0 ? parsed : this.state.offset;
            this.state.onchange?.();
          },
        }),
      ),
    });

    return {
      info: 'Limits the number of rows returned and optionally skips rows. Use LIMIT to cap results and OFFSET to skip the first N rows. Useful for pagination or sampling data.',
      sections,
    };
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('limit_and_offset');
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
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

    return true;
  }

  clone(): QueryNode {
    const stateCopy: LimitAndOffsetNodeState = {
      limit: this.state.limit,
      offset: this.state.offset,
      filters: this.state.filters?.map((f) => ({...f})),
      filterOperator: this.state.filterOperator,
      onchange: this.state.onchange,
    };
    return new LimitAndOffsetNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.primaryInput === undefined) return undefined;

    const hasLimit = this.state.limit !== undefined && this.state.limit >= 0;
    const hasOffset = this.state.offset !== undefined && this.state.offset > 0;

    if (!hasLimit && !hasOffset) {
      return this.primaryInput.getStructuredQuery();
    }

    return StructuredQueryBuilder.withLimitOffset(
      this.primaryInput,
      this.state.limit,
      this.state.offset,
      this.nodeId,
    );
  }

  serializeState(): object {
    // Only return serializable fields, excluding callbacks and objects
    // that might contain circular references
    return {
      primaryInputId: this.primaryInput?.nodeId,
      limit: this.state.limit,
      offset: this.state.offset,
    };
  }

  static deserializeState(
    state: LimitAndOffsetNodeState,
  ): LimitAndOffsetNodeState {
    return {...state};
  }
}
