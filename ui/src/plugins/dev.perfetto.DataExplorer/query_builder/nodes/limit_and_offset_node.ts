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
import {QueryNode, nextNodeId, NodeType, NodeContext} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {setValidationError} from '../node_issues';
import {InlineField} from '../widgets';
import {NodeDetailsAttrs, NodeModifyAttrs} from '../../node_types';
import {createErrorSections} from '../widgets';
import {loadNodeDoc} from '../node_doc_loader';

// Serializable node configuration.
export interface LimitAndOffsetNodeAttrs {
  limit?: number;
  offset?: number;
}

export class LimitAndOffsetNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kLimitAndOffset;
  primaryInput?: QueryNode;
  nextNodes: QueryNode[];
  readonly attrs: LimitAndOffsetNodeAttrs;
  readonly context: NodeContext;

  constructor(attrs: LimitAndOffsetNodeAttrs, context: NodeContext) {
    this.nodeId = nextNodeId();
    this.attrs = {
      ...attrs,
      limit: attrs.limit ?? 10,
      offset: attrs.offset ?? 0,
    };
    this.context = context;
    this.nextNodes = [];
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
    const hasOffset = this.attrs.offset !== undefined && this.attrs.offset > 0;
    const limitText = `Limit: ${this.attrs.limit ?? 10}`;
    const offsetText = hasOffset ? `, Offset: ${this.attrs.offset}` : '';

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
          value: this.attrs.limit?.toString() ?? '10',
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
            this.attrs.limit =
              !isNaN(parsed) && parsed >= 0 ? parsed : this.attrs.limit;
            this.context.onchange?.();
          },
        }),
        m(InlineField, {
          label: 'Offset',
          icon: 'skip_next',
          value: this.attrs.offset?.toString() ?? '0',
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
            this.attrs.offset =
              !isNaN(parsed) && parsed >= 0 ? parsed : this.attrs.offset;
            this.context.onchange?.();
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
    if (this.context.issues) {
      this.context.issues.clear();
    }

    if (this.primaryInput === undefined) {
      setValidationError(this.context, 'No input node connected');
      return false;
    }

    if (!this.primaryInput.validate()) {
      setValidationError(this.context, 'Previous node is invalid');
      return false;
    }

    return true;
  }

  clone(): QueryNode {
    return new LimitAndOffsetNode(
      {
        limit: this.attrs.limit,
        offset: this.attrs.offset,
      },
      this.context,
    );
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.primaryInput === undefined) return undefined;

    const hasLimit = this.attrs.limit !== undefined && this.attrs.limit >= 0;
    const hasOffset = this.attrs.offset !== undefined && this.attrs.offset > 0;

    if (!hasLimit && !hasOffset) {
      // No limit/offset - return passthrough to maintain reference chain
      return StructuredQueryBuilder.passthrough(this.primaryInput, this.nodeId);
    }

    return StructuredQueryBuilder.withLimitOffset(
      this.primaryInput,
      this.attrs.limit,
      this.attrs.offset,
      this.nodeId,
    );
  }
}
