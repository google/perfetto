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
import {Query, QueryNode} from '../query_node';
import {isAQuery, queryToRun} from './query_builder_utils';
import {Trace} from '../../../public/trace';
import {SqlSourceNode} from './nodes/sources/sql_source';
import {CodeSnippet} from '../../../widgets/code_snippet';
import {AggregationNode} from './nodes/aggregation_node';
import {NodeIssues} from './node_issues';
import {TabStrip} from '../../../widgets/tabs';
import {NodeModifyAttrs} from './node_explorer_types';
import {Button, ButtonAttrs, ButtonVariant} from '../../../widgets/button';
import {DataExplorerEmptyState, InfoBox} from './widgets';
import {QueryExecutionService} from './query_execution_service';

export interface NodeExplorerAttrs {
  readonly node?: QueryNode;
  readonly trace: Trace;
  readonly queryExecutionService: QueryExecutionService;
  /** Called when analysis completes (with query, error, or undefined if skipped) */
  readonly onQueryAnalyzed: (query: Query | Error | undefined) => void;
  readonly onAnalysisStateChange?: (isAnalyzing: boolean) => void;
  /** Called when execution starts */
  readonly onExecutionStart?: () => void;
  /** Called when execution succeeds */
  readonly onExecutionSuccess?: (result: {
    tableName: string;
    rowCount: number;
    columns: string[];
    durationMs: number;
  }) => void;
  /** Called when execution fails */
  readonly onExecutionError?: (error: unknown) => void;
  readonly onchange?: () => void;
  readonly resolveNode: (nodeId: string) => QueryNode | undefined;
  readonly isCollapsed?: boolean;
  readonly selectedView?: number;
  readonly onViewChange?: (view: number) => void;
  /** Whether there's already a result displayed (for reuse optimization) */
  readonly hasExistingResult?: boolean;
}

enum SelectedView {
  kInfo = 0,
  kModify = 1,
  kResult = 2,
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
    // TODO: Re-implement WITH statement dependencies for SqlSourceNode
    // This was removed during the connection model migration
    if (node instanceof SqlSourceNode && node.state.sql) {
      // Validate that the node doesn't reference itself
      const nodeIds = node.findDependencies();
      for (const nodeId of nodeIds) {
        if (nodeId === node.nodeId) {
          node.state.issues = new NodeIssues();
          node.state.issues.queryError = new Error(
            'Node cannot depend on itself',
          );
          return;
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
      // Clear prevSqString so that when node becomes valid again, we'll re-process it
      this.prevSqString = undefined;
      return;
    }

    const curSqString = JSON.stringify(sq.toJSON(), null, 2);

    if (curSqString !== this.prevSqString || node.state.hasOperationChanged) {
      if (node.state.hasOperationChanged) {
        node.state.hasOperationChanged = false;
      }

      // Use the centralized service to handle analysis and execution.
      // The service decides whether to analyze/execute based on autoExecute flag.
      this.tableAsyncLimiter.schedule(async () => {
        try {
          const result = await attrs.queryExecutionService.processNode(
            node,
            attrs.trace.engine,
            {
              manual: false, // This is automatic processing, not manual "Run Query"
              hasExistingResult: attrs.hasExistingResult,
              onAnalysisStart: () => attrs.onAnalysisStateChange?.(true),
              onAnalysisComplete: (query) => {
                this.currentQuery = query;
                if (isAQuery(query) && node instanceof AggregationNode) {
                  node.updateGroupByColumns();
                }
                attrs.onQueryAnalyzed(query);
                this.prevSqString = curSqString;
                attrs.onAnalysisStateChange?.(false);
              },
              onExecutionStart: () => attrs.onExecutionStart?.(),
              onExecutionSuccess: (result) =>
                attrs.onExecutionSuccess?.(result),
              onExecutionError: (error) => attrs.onExecutionError?.(error),
            },
          );

          // If skipped (autoExecute=false), update tracking but don't notify
          if (result.query === undefined) {
            this.prevSqString = curSqString;
          }
        } catch (e) {
          // Silently handle "Already analyzing" errors - the AsyncLimiter
          // will retry when the current analysis completes
          if (e instanceof Error && e.message.includes('Already analyzing')) {
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

  private renderButtons(
    buttons?: NodeModifyAttrs['topLeftButtons'],
  ): m.Children {
    if (!buttons || buttons.length === 0) {
      return [];
    }
    const result: m.Children = [];
    for (const btn of buttons) {
      const attrs: Partial<ButtonAttrs> = {
        onclick: btn.onclick,
        variant: btn.variant ?? ButtonVariant.Outlined, // Default to Outlined
      };
      if (btn.label) attrs.label = btn.label;
      if (btn.icon) attrs.icon = btn.icon;
      if (btn.compact !== undefined) attrs.compact = btn.compact;
      result.push(m(Button, attrs as ButtonAttrs));
    }
    return result;
  }

  private renderTopButtons(buttons: NodeModifyAttrs): m.Child {
    if (!buttons.topLeftButtons && !buttons.topRightButtons) {
      return null;
    }

    return m('.pf-exp-node-explorer__buttons-top-container', [
      m('.pf-exp-node-explorer__buttons-top', [
        m(
          '.pf-exp-node-explorer__buttons-top-left',
          this.renderButtons(buttons.topLeftButtons),
        ),
        m(
          '.pf-exp-node-explorer__buttons-top-right',
          this.renderButtons(buttons.topRightButtons),
        ),
      ]),
    ]);
  }

  private renderBottomButtons(buttons: NodeModifyAttrs): m.Child {
    if (!buttons.bottomLeftButtons && !buttons.bottomRightButtons) {
      return null;
    }

    return m('.pf-exp-node-explorer__buttons-bottom', [
      m(
        '.pf-exp-node-explorer__buttons-bottom-left',
        this.renderButtons(buttons.bottomLeftButtons),
      ),
      m(
        '.pf-exp-node-explorer__buttons-bottom-right',
        this.renderButtons(buttons.bottomRightButtons),
      ),
    ]);
  }

  private renderSections(sections?: NodeModifyAttrs['sections']): m.Child {
    if (!sections || sections.length === 0) {
      return null;
    }

    return m(
      '.pf-exp-node-explorer__sections',
      sections.map((section) =>
        m(
          '.pf-exp-node-explorer__section',
          section.title &&
            m('h3.pf-exp-node-explorer__section-title', section.title),
          m('.pf-exp-node-explorer__section-content', section.content),
        ),
      ),
    );
  }

  private renderModifyView(node: QueryNode): m.Child {
    const modifyResult = node.nodeSpecificModify();

    // Check if node returned attrs (new pattern) or m.Child (old pattern)
    if (this.isNodeModifyAttrs(modifyResult)) {
      const attrs = modifyResult as NodeModifyAttrs;
      return m('.pf-exp-node-explorer__modify', [
        m(InfoBox, attrs.info),
        this.renderTopButtons(attrs),
        this.renderSections(attrs.sections),
        this.renderBottomButtons(attrs),
      ]);
    }

    // Fallback to old pattern for backwards compatibility
    return modifyResult as m.Child;
  }

  private isNodeModifyAttrs(value: unknown): value is NodeModifyAttrs {
    if (value === null || value === undefined) return false;
    if (typeof value !== 'object') return false;

    const obj = value as Record<string, unknown>;

    // Check if it has the required info property
    return 'info' in obj;
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
      selectedView === SelectedView.kModify && this.renderModifyView(node),
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
              : m(DataExplorerEmptyState, {
                  icon: 'info',
                  title: 'SQL not available',
                })
            : isAQuery(this.currentQuery)
              ? m(CodeSnippet, {text: textproto, language: 'textproto'})
              : m(DataExplorerEmptyState, {
                  icon: 'info',
                  title: 'Proto not available',
                }),
        ]),
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

    // Process the node via the centralized service.
    // The service handles all autoExecute logic internally:
    // - If autoExecute=true: Analyze and execute automatically
    // - If autoExecute=false: Skip until user clicks "Run Query"
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
