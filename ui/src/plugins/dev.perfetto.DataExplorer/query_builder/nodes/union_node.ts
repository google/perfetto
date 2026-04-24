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
  nextNodeId,
  NodeType,
  SecondaryInputSpec,
  NodeContext,
} from '../../query_node';
import {notifyNextNodes} from '../graph_utils';
import protos from '../../../../protos';
import {ColumnInfo, newColumnInfo} from '../column_info';
import {Callout} from '../../../../widgets/callout';
import {NodeIssues} from '../node_issues';
import {StructuredQueryBuilder, ColumnSpec} from '../structured_query_builder';
import {loadNodeDoc} from '../node_doc_loader';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../../node_types';
import {ResultsPanelEmptyState} from '../widgets';
import {ColumnSelector} from '../column_selector';
import {
  NodeDetailsMessage,
  NodeTitle,
  ColumnName,
} from '../node_styling_widgets';

// Serializable node configuration.
export interface UnionNodeAttrs {
  selectedColumns: ColumnInfo[];
}

export class UnionNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kUnion;
  secondaryInputs: SecondaryInputSpec;
  nextNodes: QueryNode[];
  readonly attrs: UnionNodeAttrs;
  readonly context: NodeContext;

  get inputNodesList(): QueryNode[] {
    return [...this.secondaryInputs.connections.entries()]
      .sort(([a], [b]) => a - b)
      .map(([, node]) => node);
  }

  get finalCols(): ColumnInfo[] {
    return this.attrs.selectedColumns.filter((col) => col.checked);
  }

  constructor(
    attrs: UnionNodeAttrs & {inputNodes?: QueryNode[]},
    context: NodeContext,
  ) {
    this.nodeId = nextNodeId();
    const {inputNodes, ...rest} = attrs;
    this.attrs = rest as UnionNodeAttrs;
    this.context = context;
    this.secondaryInputs = {
      connections: new Map(),
      min: 2,
      max: 'unbounded',
      portNames: (portIndex: number) => `Input ${portIndex}`,
    };
    // Initialize connections from inputNodes
    if (inputNodes) {
      for (let i = 0; i < inputNodes.length; i++) {
        this.secondaryInputs.connections.set(i, inputNodes[i]);
      }
    }
    this.nextNodes = [];

    const userOnChange = this.context.onchange;
    this.context.onchange = () => {
      notifyNextNodes(this);
      userOnChange?.();
    };
  }

  onPrevNodesUpdated() {
    const newCommonColumns = this.getCommonColumns();

    // Preserve checked status for columns that still exist.
    for (const oldCol of this.attrs.selectedColumns ?? []) {
      const newCol = newCommonColumns.find((c) => c.name === oldCol.name);
      if (newCol) {
        newCol.checked = oldCol.checked;
      }
    }

    this.attrs.selectedColumns = newCommonColumns;
  }

  private getCommonColumns(): ColumnInfo[] {
    if (this.inputNodesList.length === 0) {
      return [];
    }
    // Filter out undefined entries before processing
    const validPrevNodes = this.inputNodesList.filter(
      (node): node is QueryNode => node !== undefined,
    );
    if (validPrevNodes.length === 0) {
      return [];
    }
    let commonCols = validPrevNodes[0].finalCols.map((col) =>
      newColumnInfo(col, true),
    );
    for (let i = 1; i < validPrevNodes.length; i++) {
      const currentNodeCols = validPrevNodes[i].finalCols;
      commonCols = commonCols.filter((commonCol) =>
        currentNodeCols.some(
          (currentNodeCol) => currentNodeCol.name === commonCol.name,
        ),
      );
    }
    return commonCols;
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.context.issues) {
      this.context.issues.clear();
    }

    // Check for undefined entries (disconnected inputs)
    const validPrevNodes = this.inputNodesList.filter(
      (node): node is QueryNode => node !== undefined,
    );

    if (validPrevNodes.length < this.inputNodesList.length) {
      this.setValidationError(
        'Union node has disconnected inputs. Please connect all inputs or remove this node.',
      );
      return false;
    }

    if (this.inputNodesList.length < 2) {
      this.setValidationError('Union node requires at least two sources.');
      return false;
    }

    if (this.getCommonColumns().length === 0) {
      this.setValidationError(
        'Union node requires common columns between sources.',
      );
      return false;
    }

    for (const inputNode of this.inputNodesList) {
      // Skip undefined entries (already handled above)
      if (inputNode === undefined) continue;

      if (!inputNode.validate()) {
        this.setValidationError(
          inputNode.context.issues?.queryError?.message ??
            `Input node '${inputNode.getTitle()}' is invalid`,
        );
        return false;
      }
    }

    return true;
  }

  private setValidationError(message: string): void {
    if (!this.context.issues) {
      this.context.issues = new NodeIssues();
    }
    this.context.issues.queryError = new Error(message);
  }

  getTitle(): string {
    return 'Union';
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('union');
  }

  nodeDetails(): NodeDetailsAttrs {
    const selectedCols = this.attrs.selectedColumns.filter((c) => c.checked);
    let message: m.Child;

    if (selectedCols.length === 0) {
      message = NodeDetailsMessage('No common columns selected');
    } else if (selectedCols.length > 3) {
      // Show the count of common columns
      message = m('div', `${selectedCols.length} common columns`);
    } else {
      // Show individual column names
      const selectedItems = selectedCols.map((c) =>
        m('div', ColumnName(c.name)),
      );
      message = m('div', ...selectedItems);
    }

    const content = [NodeTitle(this.getTitle()), message];
    return {content};
  }

  nodeSpecificModify(): NodeModifyAttrs {
    this.validate();
    const error = this.context.issues?.queryError;

    const selectedCount = this.attrs.selectedColumns.filter(
      (col) => col.checked,
    ).length;
    const totalCount = this.attrs.selectedColumns.length;

    const sections: NodeModifyAttrs['sections'] = [];

    // Add error if present
    if (error) {
      sections.push({
        content: m(Callout, {icon: 'error'}, error.message),
      });
    }

    // Selected columns section
    if (totalCount === 0) {
      // Show empty state when no common columns
      sections.push({
        content: m(ResultsPanelEmptyState, {
          icon: 'table',
          title: 'No common columns between sources',
          variant: 'warning',
        }),
      });
    } else {
      sections.push({
        title: `Select Common Columns (${selectedCount} / ${totalCount} selected)`,
        content: m(ColumnSelector, {
          columns: this.attrs.selectedColumns,
          onColumnsChange: (columns) => {
            this.attrs.selectedColumns = columns;
            this.context.onchange?.();
          },
          helpText: 'Select which common columns to include in the union',
          draggable: true,
        }),
      });
    }

    return {
      info: 'Stacks rows from multiple inputs vertically (UNION ALL). All inputs must have compatible column schemas. Useful for combining similar data from different sources.',
      sections,
    };
  }

  clone(): QueryNode {
    return new UnionNode(
      {selectedColumns: this.attrs.selectedColumns.map((c) => ({...c}))},
      this.context,
    );
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.inputNodesList.length < 2) return undefined;

    // Check for undefined entries
    for (const inputNode of this.inputNodesList) {
      if (inputNode === undefined) return undefined;
    }

    // Get the list of checked common columns
    const selectedColumns = this.attrs.selectedColumns.filter((c) => c.checked);
    if (selectedColumns.length === 0) return undefined;

    // Build column specifications for the SELECT
    const columnSpecs: ColumnSpec[] = selectedColumns.map((col) => ({
      columnNameOrExpression: col.name,
    }));

    // Create wrapper queries for each input that selects only the common columns
    // Pass the query protos directly to withUnion (not nodes)
    const wrappedQueries: protos.PerfettoSqlStructuredQuery[] = [];
    for (const inputNode of this.inputNodesList) {
      const selectQuery = StructuredQueryBuilder.withSelectColumns(
        inputNode,
        columnSpecs,
        undefined,
      );
      if (!selectQuery) return undefined;
      wrappedQueries.push(selectQuery);
    }

    // Create the union from the wrapped queries
    return StructuredQueryBuilder.withUnion(wrappedQueries, true, this.nodeId);
  }
}
