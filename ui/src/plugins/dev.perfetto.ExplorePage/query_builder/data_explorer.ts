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
import {QueryResponse} from '../../../components/query_table/queries';
import {DataGridDataSource} from '../../../components/widgets/data_grid/common';
import {DataGrid} from '../../../components/widgets/data_grid/data_grid';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Spinner} from '../../../widgets/spinner';
import {Switch} from '../../../widgets/switch';
import {Query, QueryNode, isAQuery} from '../query_node';
import {Intent} from '../../../widgets/common';
import {Icons} from '../../../base/semantic_icons';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {Icon} from '../../../widgets/icon';
import {Tooltip} from '../../../widgets/tooltip';
import {findErrors} from './query_builder_utils';
import {UIFilter, normalizeDataGridFilter} from './operations/filter';
import {DataExplorerEmptyState} from './widgets';

export interface DataExplorerAttrs {
  readonly node: QueryNode;
  readonly query?: Query | Error;
  readonly response?: QueryResponse;
  readonly dataSource?: DataGridDataSource;
  readonly isQueryRunning: boolean;
  readonly isAnalyzing: boolean;
  readonly isFullScreen: boolean;
  readonly onFullScreenToggle: () => void;
  readonly onExecute: () => void;
  readonly onExportToTimeline?: () => void;
  readonly onchange?: () => void;
  readonly onFilterAdd?: (
    filter: UIFilter | UIFilter[],
    filterOperator?: 'AND' | 'OR',
  ) => void;
}

export class DataExplorer implements m.ClassComponent<DataExplorerAttrs> {
  view({attrs}: m.CVnode<DataExplorerAttrs>) {
    return m(
      '.pf-exp-data-explorer',
      m(
        '.pf-exp-data-explorer__header',
        m(
          '.pf-exp-data-explorer__title-row',
          m('h2', 'Query data'),
          m('.pf-exp-data-explorer__buttons', this.renderMenu(attrs)),
        ),
      ),
      m('.pf-exp-data-explorer__content', this.renderContent(attrs)),
    );
  }

  private renderMenu(attrs: DataExplorerAttrs): m.Children {
    const autoExecute = attrs.node.state.autoExecute ?? true;

    const runButton =
      !autoExecute &&
      m(Button, {
        label: 'Run Query',
        icon: 'play_arrow',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        disabled: !isAQuery(attrs.query) || !attrs.node.validate(),
        onclick: () => attrs.onExecute(),
      });

    // Show "Queued..." when analyzing (validating query)
    // Show spinner when actually executing the query
    const statusIndicator =
      attrs.isAnalyzing && !attrs.isQueryRunning
        ? m('span.status-indicator', 'Queued...')
        : attrs.isQueryRunning
          ? m(Spinner)
          : null;

    const autoExecuteSwitch = m(Switch, {
      label: 'Auto Execute',
      checked: autoExecute,
      onchange: (e: Event) => {
        const target = e.target as HTMLInputElement;
        attrs.node.state.autoExecute = target.checked;
        attrs.onchange?.();
      },
    });

    // Add materialization indicator icon with tooltip
    const materializationIndicator =
      attrs.node.state.materialized && attrs.node.state.materializationTableName
        ? m(
            Tooltip,
            {
              trigger: m(Icon, {icon: 'database'}),
            },
            `Materialized as ${attrs.node.state.materializationTableName}`,
          )
        : null;

    // Helper to create separator dot
    const separator = () =>
      m(
        'span.pf-query-stats-separator',
        {
          'aria-hidden': 'true',
        },
        '•',
      );

    // Add query stats display (row count and duration)
    const queryStats =
      attrs.response && !attrs.isQueryRunning
        ? m('.pf-query-stats', [
            m('span', `${attrs.response.totalRowCount.toLocaleString()} rows`),
            separator(),
            m('span', `${attrs.response.durationMs.toFixed(1)}ms`),
          ])
        : null;

    const positionMenu = m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon: Icons.ContextMenuAlt,
        }),
      },
      [
        m(MenuItem, {
          label: 'Export to Timeline',
          icon: 'open_in_new',
          onclick: () => attrs.onExportToTimeline?.(),
          title: 'Export query results to timeline tab',
          disabled: !(
            attrs.onExportToTimeline &&
            attrs.response &&
            !attrs.isQueryRunning &&
            attrs.node.state.materialized
          ),
        }),
      ],
    );

    return [
      runButton,
      statusIndicator,
      queryStats,
      queryStats !== null && materializationIndicator !== null
        ? separator()
        : null,
      materializationIndicator,
      materializationIndicator !== null ? separator() : null,
      autoExecuteSwitch,
      positionMenu,
    ];
  }

  private renderContent(attrs: DataExplorerAttrs): m.Children {
    const errors = findErrors(attrs.query, attrs.response);

    // Show validation errors first (queryError is set by validate() methods).
    // Validation errors take priority over execution errors because if validation
    // fails, we should not execute the query at all.
    if (!attrs.node.validate() && attrs.node.state.issues?.queryError) {
      // Clear any stale execution error when validation fails
      attrs.node.state.issues.clearExecutionError();
      return m(DataExplorerEmptyState, {
        icon: 'warning',
        variant: 'warning',
        title: attrs.node.state.issues.queryError.message,
      });
    }

    // Show execution errors (e.g., when materialization fails due to
    // invalid column names). These are stored separately from validation errors
    // so they survive validate() calls during rendering.
    if (attrs.node.state.issues?.executionError) {
      return m(
        DataExplorerEmptyState,
        {
          icon: 'warning',
          variant: 'warning',
          title: attrs.node.state.issues.executionError.message,
        },
        m(Button, {
          label: 'Retry',
          icon: 'refresh',
          intent: Intent.Primary,
          onclick: () => {
            // Clear the execution error and re-run the query
            attrs.node.state.issues?.clearExecutionError();
            attrs.onExecute();
          },
        }),
      );
    }

    // Show execution errors with centered warning icon
    if (errors) {
      return m(DataExplorerEmptyState, {
        icon: 'warning',
        variant: 'warning',
        title: `Error: ${errors.message}`,
      });
    }

    // Show response warnings with centered warning icon
    if (attrs.node.state.issues?.responseError) {
      return m(DataExplorerEmptyState, {
        icon: 'warning',
        variant: 'warning',
        title: attrs.node.state.issues.responseError.message,
      });
    }

    // Show data errors (like "no rows returned") with centered warning icon
    if (attrs.node.state.issues?.dataError) {
      return m(DataExplorerEmptyState, {
        icon: 'warning',
        variant: 'warning',
        title: attrs.node.state.issues.dataError.message,
      });
    }

    // Show spinner overlay when query is running
    if (attrs.isQueryRunning) {
      return m(DataExplorerEmptyState, {}, m(Spinner, {easing: true}));
    }

    // Show "No data to display" when no query is available
    if (attrs.query === undefined) {
      return m(DataExplorerEmptyState, {
        title: 'No data to display',
      });
    }

    if (attrs.response && attrs.dataSource && attrs.node.validate()) {
      // Show warning for multiple statements with centered icon
      const warning =
        attrs.response.statementWithOutputCount > 1
          ? m(DataExplorerEmptyState, {
              icon: 'warning',
              variant: 'warning',
              title:
                `${attrs.response.statementWithOutputCount} out of ${attrs.response.statementCount} ` +
                'statements returned a result. ' +
                'Only the results for the last statement are displayed.',
            })
          : null;

      const supportedOps = [
        '=',
        '!=',
        '<',
        '<=',
        '>',
        '>=',
        'glob',
        'in',
        'not in',
        'is null',
        'is not null',
      ] as const;

      return [
        warning,
        m(DataGrid, {
          fillHeight: true,
          columns: attrs.response.columns.map((c) => ({name: c})),
          data: attrs.dataSource,
          showFiltersInToolbar: true,
          supportedFilters: supportedOps,
          // We don't actually want the datagrid to display or apply any filters
          // to the datasource itself, so we define this but fix it as an empty
          // array.
          filters: [],
          onFilterAdd: (filter) => {
            // Normalize the filter (expands IN/NOT IN to multiple equality filters)
            const normalizedFilters = normalizeDataGridFilter(filter);

            if (attrs.onFilterAdd) {
              // Pass all normalized filters at once
              // Determine logical operator based on original filter type:
              // - IN: multiple values ORed together (value = X OR value = Y)
              // - NOT IN: multiple values ANDed together (value != X AND value != Y)
              //   (De Morgan's law: NOT(A OR B) = NOT A AND NOT B)
              let operator: 'AND' | 'OR' | undefined;
              if (normalizedFilters.length > 1) {
                operator = filter.op === 'not in' ? 'AND' : 'OR';
              }
              attrs.onFilterAdd(
                normalizedFilters.length === 1
                  ? normalizedFilters[0]
                  : normalizedFilters,
                operator,
              );
            } else {
              // Legacy: add filters directly to node state
              attrs.node.state.filters = [
                ...(attrs.node.state.filters ?? []),
                ...normalizedFilters,
              ];
              if (normalizedFilters.length > 1) {
                attrs.node.state.filterOperator =
                  filter.op === 'not in' ? 'AND' : 'OR';
              }
            }
            attrs.onchange?.();
          },
        }),
      ];
    }

    // Show a prominent execute button when query is ready but not executed
    const autoExecute = attrs.node.state.autoExecute ?? true;
    if (
      !autoExecute &&
      isAQuery(attrs.query) &&
      !attrs.response &&
      !attrs.isQueryRunning &&
      !attrs.isAnalyzing
    ) {
      return m(
        DataExplorerEmptyState,
        {},
        m(Button, {
          label: 'Run Query',
          icon: 'play_arrow',
          intent: Intent.Primary,
          variant: ButtonVariant.Filled,
          onclick: () => attrs.onExecute(),
        }),
      );
    }

    return null;
  }
}
