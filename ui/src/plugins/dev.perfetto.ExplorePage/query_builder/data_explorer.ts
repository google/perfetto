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
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {
  CellRenderer,
  ColumnSchema,
  SchemaRegistry,
} from '../../../components/widgets/datagrid/datagrid_schema';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Spinner} from '../../../widgets/spinner';
import {Switch} from '../../../widgets/switch';
import {Query, QueryNode} from '../query_node';
import {Intent} from '../../../widgets/common';
import {Icons} from '../../../base/semantic_icons';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {findErrors} from './query_builder_utils';
import {UIFilter, normalizeDataGridFilter} from './operations/filter';
import {DataExplorerEmptyState} from './widgets';
import {Trace} from '../../../public/trace';
import {Timestamp} from '../../../components/widgets/timestamp';
import {DurationWidget} from '../../../components/widgets/duration';
import {Time, Duration} from '../../../base/time';
import {ColumnInfo} from './column_info';
import {DetailsShell} from '../../../widgets/details_shell';
import {DataSource} from '../../../components/widgets/datagrid/data_source';

export interface DataExplorerAttrs {
  readonly trace: Trace;
  readonly node: QueryNode;
  readonly query?: Query | Error;
  readonly response?: QueryResponse;
  readonly dataSource?: DataSource;
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

// Create cell renderer for timestamp columns
function createTimestampCellRenderer(trace: Trace): CellRenderer {
  return (value) => {
    if (typeof value === 'number') {
      value = BigInt(Math.round(value));
    }
    if (typeof value !== 'bigint') {
      return String(value);
    }
    return m(Timestamp, {
      trace,
      ts: Time.fromRaw(value),
    });
  };
}

// Create cell renderer for duration columns
function createDurationCellRenderer(trace: Trace): CellRenderer {
  return (value) => {
    if (typeof value === 'number') {
      value = BigInt(Math.round(value));
    }
    if (typeof value !== 'bigint') {
      return String(value);
    }
    return m(DurationWidget, {
      trace,
      dur: Duration.fromRaw(value),
    });
  };
}

// Get column info by name from the node's finalCols
function getColumnInfo(
  node: QueryNode,
  columnName: string,
): ColumnInfo | undefined {
  return node.finalCols.find((col) => col.name === columnName);
}

export class DataExplorer implements m.ClassComponent<DataExplorerAttrs> {
  view({attrs}: m.CVnode<DataExplorerAttrs>) {
    return m(
      DetailsShell,
      {
        title: 'Query data',
        buttons: this.renderMenu(attrs),
        fillHeight: true,
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
        disabled: !attrs.node.validate(),
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
        // Execute the query when auto-execute is toggled on
        // Analysis will happen automatically in node_explorer when autoExecute becomes true
        if (target.checked && attrs.node.validate()) {
          attrs.onExecute();
        }
      },
    });

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
        m(MenuItem, {
          label: 'Copy Materialized Table Name',
          icon: 'content_copy',
          onclick: () => {
            const tableName = attrs.node.state.materializationTableName;
            if (tableName) {
              navigator.clipboard.writeText(tableName);
            }
          },
          title: 'Copy the materialized table name to clipboard',
          disabled: !(
            attrs.node.state.materialized &&
            attrs.node.state.materializationTableName
          ),
        }),
      ],
    );

    // Collect all items that should have separators between them
    const itemsWithSeparators = [
      runButton,
      statusIndicator,
      queryStats,
      autoExecuteSwitch,
    ].filter((item) => item !== null && item !== false);

    // Add separators between items
    const menuItems: m.Children = [];
    for (let i = 0; i < itemsWithSeparators.length; i++) {
      menuItems.push(itemsWithSeparators[i]);
      if (i < itemsWithSeparators.length - 1) {
        menuItems.push(separator());
      }
    }

    // Add menu at the end without a separator
    menuItems.push(positionMenu);

    return menuItems;
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

    // Show data if we have response and dataSource (even without query)
    // This handles the case where we load existing materialized data
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

      // Build schema directly
      const columnSchema: ColumnSchema = {};
      for (const c of attrs.response.columns) {
        let cellRenderer: CellRenderer | undefined;

        // Get column type information from the node
        const columnInfo = getColumnInfo(attrs.node, c);
        if (columnInfo) {
          // Check if this is a timestamp column
          if (columnInfo.type === 'TIMESTAMP') {
            cellRenderer = createTimestampCellRenderer(attrs.trace);
          }
          // Check if this is a duration column
          else if (columnInfo.type === 'DURATION') {
            cellRenderer = createDurationCellRenderer(attrs.trace);
          }
        }

        columnSchema[c] = {cellRenderer};
      }
      const schema: SchemaRegistry = {data: columnSchema};

      return [
        warning,
        m(DataGrid, {
          schema,
          rootSchema: 'data',
          initialColumns: attrs.response.columns.map((col) => ({field: col})),
          fillHeight: true,
          data: attrs.dataSource,
          enablePivotControls: false,
          structuredQueryCompatMode: true,
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

    // Show a prominent execute button when autoExecute is false and not yet executed
    const autoExecute = attrs.node.state.autoExecute ?? true;
    if (
      !autoExecute &&
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
          disabled: !attrs.node.validate(),
          onclick: () => attrs.onExecute(),
        }),
      );
    }

    // Show "No data to display" when no response is available
    // (for autoExecute=true nodes that haven't run yet)
    if (!attrs.response) {
      return m(DataExplorerEmptyState, {
        title: 'No data to display',
      });
    }

    return null;
  }
}
