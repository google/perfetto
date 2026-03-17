// Copyright (C) 2026 The Android Open Source Project
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
import protos from '../../../../protos';
import {
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import {NodeIssues} from '../node_issues';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../../node_types';
import {loadNodeDoc} from '../node_doc_loader';
import {NodeTitle} from '../node_styling_widgets';
import {InlineField, ResultsPanelEmptyState} from '../widgets';
import {
  dashboardRegistry,
  DashboardDataSource,
} from '../../dashboard/dashboard_registry';

export interface DashboardSerializedState {
  exportName?: string;
}

export interface DashboardNodeState extends QueryNodeState {
  // User-assigned name for this exported data source.
  exportName?: string;
  // Stable ID of the graph tab that owns this node (set by DataExplorer).
  graphId?: string;
}

/** Type guard: returns true if the given node is a DashboardNode. */
export function isDashboardNode(node: QueryNode): node is DashboardNode {
  return node.type === NodeType.kDashboard;
}

export class DashboardNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kDashboard;
  nextNodes: QueryNode[];
  primaryInput?: QueryNode;
  readonly state: DashboardNodeState;

  get finalCols(): ColumnInfo[] {
    return this.primaryInput?.finalCols ?? [];
  }

  constructor(state: DashboardNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.nextNodes = [];
  }

  private getExportName(): string {
    return (
      this.state.exportName?.trim() ||
      this.primaryInput?.getTitle() ||
      'Unnamed export'
    );
  }

  validate(): boolean {
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.primaryInput === undefined) {
      this.setValidationError('No input connected');
      return false;
    }

    if (!this.primaryInput.validate()) {
      this.setValidationError(
        `Input '${this.primaryInput.getTitle()}' is invalid`,
      );
      return false;
    }

    return true;
  }

  private setValidationError(message: string): void {
    if (!this.state.issues) {
      this.state.issues = new NodeIssues();
    }
    this.state.issues.queryError = new Error(message);
  }

  getTitle(): string {
    return 'Export to Dashboard';
  }

  nodeDetails(): NodeDetailsAttrs {
    const details: m.Child[] = [NodeTitle(this.getTitle())];
    details.push(m('div', `Exported as ${this.getExportName()}`));
    return {content: details};
  }

  nodeSpecificModify(): NodeModifyAttrs {
    return {
      info: 'Export this data source to the dashboard. Give it a name so you can find it on dashboards.',
      sections: [
        {
          content: m(InlineField, {
            label: 'Export name',
            value: this.state.exportName ?? '',
            placeholder:
              this.primaryInput?.getTitle() ?? 'Name for this data source',
            onchange: (value: string) => {
              this.state.exportName = value.trim() || undefined;
              this.publishExportedSource();
              this.state.onchange?.();
            },
          }),
        },
      ],
    };
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('dashboard');
  }

  customResultsPanel(): m.Children {
    return m(ResultsPanelEmptyState, {
      icon: 'dashboard',
      title: 'Data from this node is exported to dashboards.',
    });
  }

  clone(): QueryNode {
    return new DashboardNode({
      exportName: this.state.exportName,
      onchange: this.state.onchange,
    });
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    return this.primaryInput?.getStructuredQuery();
  }

  /** Publish this node's data to the global exported sources pool. */
  private publishExportedSource(): void {
    if (this.primaryInput === undefined) {
      dashboardRegistry.removeExportedSource(this.nodeId);
      return;
    }
    const parentNodeId = this.primaryInput.nodeId;
    const source: DashboardDataSource = {
      name: this.getExportName(),
      nodeId: this.nodeId,
      columns: this.finalCols.map((c) => ({name: c.name, type: c.column.type})),
      graphId: this.state.graphId ?? '',
      requestExecution: async () => {
        await this.state.requestNodeExecution?.(parentNodeId);
        // After execution, resolve the table name so the chart can render.
        await this.resolveTableName(source, parentNodeId);
      },
    };
    dashboardRegistry.setExportedSource(source);

    // Eagerly resolve the table name. If the upstream node has already been
    // executed this returns immediately (cached in QueryExecutionService).
    this.resolveTableName(source, parentNodeId).catch((e: unknown) =>
      console.debug('Dashboard table name resolution failed:', e),
    );
  }

  private async resolveTableName(
    source: DashboardDataSource,
    parentNodeId: string,
  ): Promise<void> {
    const tableName = await this.state.getTableNameForNode?.(parentNodeId);
    if (tableName !== undefined) {
      source.tableName = tableName;
      dashboardRegistry.setExportedSource(source);
    }
  }

  onPrevNodesUpdated(): void {
    this.publishExportedSource();
  }

  serializeState(): DashboardSerializedState & {primaryInputId?: string} {
    return {
      primaryInputId: this.primaryInput?.nodeId,
      exportName: this.state.exportName,
    };
  }

  static deserializeState(state: DashboardSerializedState): DashboardNodeState {
    return {exportName: state.exportName};
  }
}
