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

import {AsyncLimiter} from '../../../base/async_limiter';
import {
  analyzeNode,
  isAQuery,
  Query,
  QueryNode,
  queryToRun,
  addConnection,
} from '../query_node';
import {Trace} from '../../../public/trace';
import {SqlSourceNode} from './nodes/sources/sql_source';
import {CodeSnippet} from '../../../widgets/code_snippet';
import {AggregationNode} from './nodes/aggregation_node';
import {NodeIssues} from './node_issues';
import {TabStrip} from '../../../widgets/tabs';

export interface NodeExplorerAttrs {
  readonly node?: QueryNode;
  readonly trace: Trace;
  readonly onQueryAnalyzed: (query: Query | Error) => void;
  readonly onAnalysisStateChange?: (isAnalyzing: boolean) => void;
  readonly onchange?: () => void;
  readonly resolveNode: (nodeId: string) => QueryNode | undefined;
  readonly isCollapsed?: boolean;
  readonly selectedView?: number;
  readonly onViewChange?: (view: number) => void;
}

enum SelectedView {
  kInfo = 0,
  kModify = 1,
  kResult = 2,
  kComment = 3,
}

export class NodeExplorer implements m.ClassComponent<NodeExplorerAttrs> {
  private readonly tableAsyncLimiter = new AsyncLimiter();

  private prevSqString?: string;

  private currentQuery?: Query | Error;
  private sqlForDisplay?: string;
  private resultTabMode: 'sql' | 'proto' = 'sql';

  private renderTitleRow(node: QueryNode): m.Child {
    return m(
      '.pf-exp-node-explorer__title-row',
      m('.title', m('h2', node.getTitle())),
    );
  }

  private updateQuery(node: QueryNode, attrs: NodeExplorerAttrs) {
    if (node instanceof SqlSourceNode && node.state.sql) {
      // Clear this node from the prevNodes
      for (const prevNode of node.prevNodes) {
        prevNode.nextNodes = prevNode.nextNodes.filter((n) => n !== node);
      }

      const nodeIds = node.findDependencies();
      const dependencies: QueryNode[] = [];
      for (const nodeId of nodeIds) {
        if (nodeId === node.nodeId) {
          node.state.issues = new NodeIssues();
          node.state.issues.queryError = new Error(
            'Node cannot depend on itself',
          );
          return;
        }

        const dependencyNode = attrs.resolveNode(nodeId);
        if (dependencyNode) {
          dependencies.push(dependencyNode);
        }
      }
      node.prevNodes = dependencies;
      for (let i = 0; i < node.prevNodes.length; i++) {
        const prevNode = node.prevNodes[i];
        if (prevNode !== undefined) {
          addConnection(prevNode, node, i);
        }
      }
    }

    const sq = node.getStructuredQuery();
    if (sq === undefined) {
      // Report error instead of silently returning
      const error = new Error(
        'Cannot generate structured query. This usually means:\n' +
          '• Multi-source nodes (Union/Merge/Intersect) need at least 2 connected inputs\n' +
          '• All input ports must be connected\n' +
          '• Previous nodes must be valid',
      );
      this.currentQuery = error;
      attrs.onQueryAnalyzed(error);
      return;
    }

    const curSqString = JSON.stringify(sq.toJSON(), null, 2);

    if (curSqString !== this.prevSqString || node.state.hasOperationChanged) {
      if (node.state.hasOperationChanged) {
        node.state.hasOperationChanged = false;
      }
      attrs.onAnalysisStateChange?.(true);
      this.tableAsyncLimiter.schedule(async () => {
        try {
          this.currentQuery = await analyzeNode(node, attrs.trace.engine);
          if (!isAQuery(this.currentQuery)) {
            attrs.onAnalysisStateChange?.(false);
            return;
          }
          if (node instanceof AggregationNode) {
            node.updateGroupByColumns();
          }
          attrs.onQueryAnalyzed(this.currentQuery);
          this.prevSqString = curSqString;
          attrs.onAnalysisStateChange?.(false);
        } catch (e) {
          // Silently handle "Already analyzing" errors - the AsyncLimiter
          // will retry when the current analysis completes
          if (e instanceof Error && e.message.includes('Already analyzing')) {
            // Keep isAnalyzing = true, will retry automatically
            return;
          }
          // For other errors, set them as the current query and stop analyzing
          const error = e instanceof Error ? e : new Error(String(e));
          this.currentQuery = error;
          attrs.onQueryAnalyzed(error);
          attrs.onAnalysisStateChange?.(false);
        }
      });
    }
  }

  private renderContent(node: QueryNode, selectedView: number): m.Child {
    const sql: string =
      this.sqlForDisplay ??
      (isAQuery(this.currentQuery)
        ? queryToRun(this.currentQuery)
        : 'SQL not available.');
    const textproto: string = isAQuery(this.currentQuery)
      ? this.currentQuery.textproto
      : this.currentQuery instanceof Error
        ? this.currentQuery.message
        : 'Proto not available.';

    return m(
      'article',
      selectedView === SelectedView.kInfo && node.nodeInfo(),
      selectedView === SelectedView.kModify && node.nodeSpecificModify(),
      selectedView === SelectedView.kResult &&
        m('.', [
          m(TabStrip, {
            tabs: [
              {key: 'sql', title: 'SQL'},
              {key: 'proto', title: 'Proto'},
            ],
            currentTabKey: this.resultTabMode,
            onTabChange: (key: string) => {
              this.resultTabMode = key as 'sql' | 'proto';
            },
          }),
          m('hr', {
            style: {
              margin: '0',
              borderTop: '1px solid var(--separator-color)',
            },
          }),
          this.resultTabMode === 'sql'
            ? isAQuery(this.currentQuery)
              ? m(CodeSnippet, {language: 'SQL', text: sql})
              : m('div', sql)
            : isAQuery(this.currentQuery)
              ? m(CodeSnippet, {text: textproto, language: 'textproto'})
              : m('div', textproto),
        ]),
      selectedView === SelectedView.kComment &&
        m('textarea.pf-exp-node-explorer__comment', {
          'aria-label': 'Comment',
          'placeholder': 'Add a comment...',
          'oninput': (e: InputEvent) => {
            if (!e.target) return;
            node.state.comment = (e.target as HTMLTextAreaElement).value;
          },
          'value': node.state.comment,
        }),
    );
  }

  view({attrs}: m.CVnode<NodeExplorerAttrs>) {
    const {node, isCollapsed, selectedView = SelectedView.kInfo} = attrs;
    if (!node) {
      return null;
    }

    // Update the node's onchange callback to point to our attrs.onchange
    // This ensures that changes in the node's UI components trigger the callback chain
    node.state.onchange = attrs.onchange;

    // Always analyze to generate the query object (needed to enable Run button)
    // The autoExecute flag only controls whether we automatically execute after analysis
    this.updateQuery(node, attrs);

    if (isCollapsed) {
      return m('.pf-exp-node-explorer.collapsed');
    }

    return m(
      `.pf-exp-node-explorer${
        node instanceof SqlSourceNode ? '.pf-exp-node-explorer-sql-source' : ''
      }`,
      m('.pf-exp-node-explorer__header', this.renderTitleRow(node)),
      this.renderContent(node, selectedView),
    );
  }
}
