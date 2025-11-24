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
import {
  DataGridDataSource,
  FilterNull,
  FilterValue,
} from '../../../components/widgets/data_grid/common';
import {
  DataGrid,
  renderCell,
} from '../../../components/widgets/data_grid/data_grid';
import {SqlValue} from '../../../trace_processor/query_result';
import {Button, ButtonVariant} from '../../../widgets/button';
import {DetailsShell} from '../../../widgets/details_shell';
import {Spinner} from '../../../widgets/spinner';
import {Switch} from '../../../widgets/switch';
import {TextParagraph} from '../../../widgets/text_paragraph';
import {Query, QueryNode, isAQuery} from '../query_node';
import {QueryService} from './query_service';
import {Intent} from '../../../widgets/common';
import {Icons} from '../../../base/semantic_icons';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {Icon} from '../../../widgets/icon';
import {Tooltip} from '../../../widgets/tooltip';
import {findErrors} from './query_builder_utils';
export interface DataExplorerAttrs {
  readonly queryService: QueryService;
  readonly node: QueryNode;
  readonly query?: Query | Error;
  readonly response?: QueryResponse;
  readonly dataSource?: DataGridDataSource;
  readonly isQueryRunning: boolean;
  readonly isAnalyzing: boolean;
  readonly isFullScreen: boolean;
  readonly onFullScreenToggle: () => void;
  readonly onExecute: () => void;
  readonly onchange?: () => void;
  readonly onFilterAdd?: (filter: FilterValue | FilterNull) => void;
}

export class DataExplorer implements m.ClassComponent<DataExplorerAttrs> {
  view({attrs}: m.CVnode<DataExplorerAttrs>) {
    return m(
      DetailsShell,
      {
        title: 'Query data',
        fillHeight: true,
        buttons: this.renderMenu(attrs),
      },
      this.renderContent(attrs),
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
          label: attrs.isFullScreen ? 'Exit full screen' : 'Full screen',
          onclick: () => attrs.onFullScreenToggle(),
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

    // Show validation errors with centered warning icon
    if (!attrs.node.validate() && attrs.node.state.issues?.queryError) {
      return m(
        '.pf-data-explorer-empty-state',
        m(Icon, {
          className: 'pf-data-explorer-warning-icon',
          icon: 'warning',
        }),
        m(
          '.pf-data-explorer-warning-message',
          attrs.node.state.issues.queryError.message,
        ),
      );
    }

    // Show execution errors with centered warning icon
    if (errors) {
      return m(
        '.pf-data-explorer-empty-state',
        m(Icon, {
          className: 'pf-data-explorer-warning-icon',
          icon: 'warning',
        }),
        m('.pf-data-explorer-warning-message', `Error: ${errors.message}`),
      );
    }

    // Show response warnings with centered warning icon
    if (attrs.node.state.issues?.responseError) {
      return m(
        '.pf-data-explorer-empty-state',
        m(Icon, {
          className: 'pf-data-explorer-warning-icon',
          icon: 'warning',
        }),
        m(
          '.pf-data-explorer-warning-message',
          attrs.node.state.issues.responseError.message,
        ),
      );
    }

    // Show data errors (like "no rows returned") with centered warning icon
    if (attrs.node.state.issues?.dataError) {
      return m(
        '.pf-data-explorer-empty-state',
        m(Icon, {
          className: 'pf-data-explorer-warning-icon',
          icon: 'warning',
        }),
        m(
          '.pf-data-explorer-warning-message',
          attrs.node.state.issues.dataError.message,
        ),
      );
    }

    // Show spinner overlay when query is running
    if (attrs.isQueryRunning) {
      return m(
        '.pf-data-explorer-empty-state',
        m(
          '.pf-exp-query-running-spinner',
          {
            style: {
              fontSize: '64px',
            },
          },
          m(Spinner, {
            easing: true,
          }),
        ),
      );
    }

    // Show "No data to display" when no query is available
    if (attrs.query === undefined) {
      return m(TextParagraph, {text: 'No data to display'});
    }

    if (attrs.response && attrs.dataSource && attrs.node.validate()) {
      // Show warning for multiple statements with centered icon
      const warning =
        attrs.response.statementWithOutputCount > 1
          ? m(
              '.pf-data-explorer-empty-state',
              m(Icon, {
                className: 'pf-data-explorer-warning-icon',
                icon: 'warning',
              }),
              m(
                '.pf-data-explorer-warning-message',
                `${attrs.response.statementWithOutputCount} out of ${attrs.response.statementCount} `,
                'statements returned a result. ',
                'Only the results for the last statement are displayed.',
              ),
            )
          : null;

      return [
        warning,
        m(DataGrid, {
          fillHeight: true,
          columns: attrs.response.columns.map((c) => ({name: c})),
          data: attrs.dataSource,
          showFiltersInToolbar: true,
          // We don't actually want the datagrid to display or apply any filters
          // to the datasource itself, so we define this but fix it as an empty
          // array.
          filters: [],
          onFilterAdd: (filter) => {
            // These are the filters supported by the explore page currently.
            const supportedOps = [
              '=',
              '!=',
              '<',
              '<=',
              '>',
              '>=',
              'glob',
              'is null',
              'is not null',
            ];
            if (supportedOps.includes(filter.op)) {
              if (attrs.onFilterAdd) {
                // Delegate to the parent handler which will create a FilterNode
                attrs.onFilterAdd(filter as FilterValue | FilterNull);
              } else {
                // Fallback: add filter directly to node state (legacy behavior)
                attrs.node.state.filters = [
                  ...(attrs.node.state.filters ?? []),
                  filter as FilterValue | FilterNull,
                ];
                attrs.onchange?.();
              }
            }
          },
          cellRenderer: (value: SqlValue, name: string) => {
            return renderCell(value, name);
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
        '.pf-data-explorer-empty-state',
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
