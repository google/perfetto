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

import {classNames} from '../../../base/classnames';
import {AsyncLimiter} from '../../../base/async_limiter';
import {ExplorePageHelp} from './help';
import {
  analyzeNode,
  isAQuery,
  Query,
  QueryNode,
  queryToRun,
} from '../query_node';
import {Button} from '../../../widgets/button';
import {Icon} from '../../../widgets/icon';
import {Icons} from '../../../base/semantic_icons';
import {Trace} from '../../../public/trace';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {SqlSourceNode} from './nodes/sources/sql_source';
import {CodeSnippet} from '../../../widgets/code_snippet';
import {AggregationNode} from './nodes/aggregation_node';
import {NodeIssues} from './node_issues';
import {Intent} from '../../../widgets/common';

export interface NodeExplorerAttrs {
  readonly node?: QueryNode;
  readonly trace: Trace;
  readonly onQueryAnalyzed: (query: Query | Error) => void;
  readonly onExecute: () => void;
  readonly onchange?: () => void;
  readonly resolveNode: (nodeId: string) => QueryNode | undefined;
}

enum SelectedView {
  kModify = 0,
  kSql = 1,
  kProto = 2,
}

export class NodeExplorer implements m.ClassComponent<NodeExplorerAttrs> {
  private readonly tableAsyncLimiter = new AsyncLimiter();

  private selectedView: number = 0;

  private prevSqString?: string;

  private currentQuery?: Query | Error;
  private sqlForDisplay?: string;

  private renderTitleRow(
    node: QueryNode,
    attrs: NodeExplorerAttrs,
    renderMenu: () => m.Child,
  ): m.Child {
    const autoExecute = node.state.autoExecute ?? true;
    return m(
      '.pf-exp-node-explorer__title-row',
      m(
        '.title',
        m(
          'h2',
          !node.validate() &&
            m(Icon, {
              icon: Icons.Warning,
              filled: true,
              className: classNames(
                'pf-exp-node-explorer__warning-icon--error',
              ),
              title: `Invalid node: \n${node.state.issues?.getTitle() ?? ''}`,
            }),
          node.getTitle(),
        ),
      ),
      m('span.spacer'), // Added spacer to push menu to the right
      !autoExecute &&
        m(Button, {
          label: 'Run',
          onclick: attrs.onExecute,
          intent: Intent.Primary,
        }),
      renderMenu(),
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
      for (const prevNode of node.prevNodes) {
        if (!prevNode.nextNodes.includes(node)) {
          prevNode.nextNodes.push(node);
        }
      }
    }

    const sq = node.getStructuredQuery();
    if (sq === undefined) return;

    const curSqString = JSON.stringify(sq.toJSON(), null, 2);

    if (curSqString !== this.prevSqString || node.state.hasOperationChanged) {
      if (node.state.hasOperationChanged) {
        node.state.hasOperationChanged = false;
      }
      this.tableAsyncLimiter.schedule(async () => {
        this.currentQuery = await analyzeNode(node, attrs.trace.engine);
        if (!isAQuery(this.currentQuery)) {
          return;
        }
        if (node instanceof AggregationNode) {
          node.updateGroupByColumns();
        }
        attrs.onQueryAnalyzed(this.currentQuery);
        this.prevSqString = curSqString;
      });
    }
  }

  private renderContent(node: QueryNode, attrs: NodeExplorerAttrs): m.Child {
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
      this.selectedView === SelectedView.kModify && [
        node.nodeSpecificModify(attrs.onExecute),
        m('textarea.pf-exp-node-explorer__comment', {
          'aria-label': 'Comment',
          'placeholder': 'Add a comment...',
          'oninput': (e: InputEvent) => {
            if (!e.target) return;
            node.state.comment = (e.target as HTMLTextAreaElement).value;
          },
          'value': node.state.comment,
        }),
      ],
      this.selectedView === SelectedView.kSql &&
        (isAQuery(this.currentQuery)
          ? m(CodeSnippet, {language: 'SQL', text: sql})
          : m('div', sql)),
      this.selectedView === SelectedView.kProto &&
        (isAQuery(this.currentQuery)
          ? m(CodeSnippet, {text: textproto, language: 'textproto'})
          : m('div', textproto)),
    );
  }

  view({attrs}: m.CVnode<NodeExplorerAttrs>) {
    const {node} = attrs;
    if (!node) {
      return m(ExplorePageHelp);
    }

    this.updateQuery(node, attrs);

    const renderModeMenu = (): m.Child => {
      return m(
        PopupMenu,
        {
          trigger: m(Button, {
            icon: Icons.ContextMenuAlt,
          }),
        },
        [
          m(MenuItem, {
            label: 'Modify',
            onclick: () => {
              this.selectedView = SelectedView.kModify;
            },
          }),
          m(MenuItem, {
            label: 'Show SQL',
            onclick: () => {
              this.selectedView = SelectedView.kSql;
            },
          }),
          m(MenuItem, {
            label: 'Show proto',
            onclick: () => {
              this.selectedView = SelectedView.kProto;
            },
          }),
        ],
      );
    };

    return m(
      `.pf-exp-node-explorer${
        node instanceof SqlSourceNode ? '.pf-exp-node-explorer-sql-source' : ''
      }`,
      this.renderTitleRow(node, attrs, renderModeMenu),
      this.renderContent(node, attrs),
    );
  }
}
