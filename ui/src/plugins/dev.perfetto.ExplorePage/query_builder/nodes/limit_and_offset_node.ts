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
import {ListItem} from '../widgets';
import {NodeDetailsAttrs, NodeModifyAttrs} from '../node_explorer_types';
import {createErrorSections} from '../widgets';
import {showModal} from '../../../../widgets/modal';
import {TextInput} from '../../../../widgets/text_input';

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

  private showEditLimitModal(): void {
    let tempValue = this.state.limit?.toString() ?? '10';

    showModal({
      title: 'Edit Limit',
      content: () =>
        m(
          'div',
          m(TextInput, {
            value: tempValue,
            type: 'number',
            oninput: (e: Event) => {
              tempValue = (e.target as HTMLInputElement).value;
            },
            placeholder: 'Number of rows',
          }),
        ),
      buttons: [
        {
          text: 'Cancel',
          action: () => {},
        },
        {
          text: 'Apply',
          primary: true,
          action: () => {
            const parsed = parseInt(tempValue.trim(), 10);
            if (!isNaN(parsed) && parsed >= 0) {
              this.state.limit = parsed;
              this.state.onchange?.();
            }
          },
        },
      ],
    });
  }

  private showEditOffsetModal(): void {
    let tempValue = this.state.offset?.toString() ?? '0';

    showModal({
      title: 'Edit Offset',
      content: () =>
        m(
          'div',
          m(TextInput, {
            value: tempValue,
            type: 'number',
            oninput: (e: Event) => {
              tempValue = (e.target as HTMLInputElement).value;
            },
            placeholder: 'Number of rows to skip',
          }),
        ),
      buttons: [
        {
          text: 'Cancel',
          action: () => {},
        },
        {
          text: 'Apply',
          primary: true,
          action: () => {
            const parsed = parseInt(tempValue.trim(), 10);
            if (!isNaN(parsed) && parsed >= 0) {
              this.state.offset = parsed;
              this.state.onchange?.();
            }
          },
        },
      ],
    });
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

    // Limit and Offset list items
    sections.push({
      content: m(
        '.pf-limit-offset-list',
        m(ListItem, {
          icon: 'filter_list',
          name: 'Limit',
          description: this.state.limit?.toString() ?? '10',
          actions: [
            {
              icon: 'edit',
              onclick: () => this.showEditLimitModal(),
            },
          ],
        }),
        m(ListItem, {
          icon: 'skip_next',
          name: 'Offset',
          description: this.state.offset?.toString() ?? '0',
          actions: [
            {
              icon: 'edit',
              onclick: () => this.showEditOffsetModal(),
            },
          ],
        }),
      ),
    });

    return {sections};
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Limit the number of rows returned and optionally skip rows. Useful for sampling data or pagination.',
      ),
      m(
        'p',
        m('strong', 'Tip:'),
        ' Combine with Sort to get meaningful results like "top 10 longest slices" or "rows 100-150".',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Set limit to 10 to see first 10 rows, or set offset to 100 and limit to 50 to see rows 100-150.',
      ),
    );
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
    return new LimitAndOffsetNode(this.state);
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
