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
import {Column} from '../../../components/widgets/datagrid/model';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Spinner} from '../../../widgets/spinner';
import {Switch} from '../../../widgets/switch';
import {Query, QueryNode} from '../query_node';
import {Intent} from '../../../widgets/common';
import {Icons} from '../../../base/semantic_icons';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {findErrors, isAQuery} from './query_builder_utils';
import {UIFilter, normalizeDataGridFilter} from './operations/filter';
import {DataExplorerEmptyState} from './widgets';
import {Trace} from '../../../public/trace';
import {Timestamp} from '../../../components/widgets/timestamp';
import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {DurationWidget} from '../../../components/widgets/duration';
import {Time, Duration} from '../../../base/time';
import {ColumnInfo} from './column_info';
import {DetailsShell} from '../../../widgets/details_shell';
import {DataSource} from '../../../components/widgets/datagrid/data_source';
import {
  PerfettoSqlType,
  isIdType,
} from '../../../trace_processor/perfetto_sql_type';
import {ColumnType} from '../../../components/widgets/datagrid/datagrid_schema';
import {QueryExecutionService} from './query_execution_service';

// Map PerfettoSqlType to DataGrid ColumnType
function getColumnType(type: PerfettoSqlType): ColumnType {
  // ID types (id, joinid, arg_set_id) should be treated as identifiers
  // They're numeric but we want distinct value pickers
  if (isIdType(type) || type.kind === 'arg_set_id' || type.kind === 'boolean') {
    return 'identifier';
  }

  // String and bytes are text types
  if (type.kind === 'string' || type.kind === 'bytes') {
    return 'text';
  }

  // All other numeric types (int, double, timestamp, duration) are quantitative
  return 'quantitative';
}

export interface DataExplorerAttrs {
  readonly trace: Trace;
  readonly node: QueryNode;
  readonly query?: Query | Error;
  readonly response?: QueryResponse;
  readonly dataSource?: DataSource;
  readonly sqlModules: SqlModules;
  readonly queryExecutionService: QueryExecutionService;
  readonly isQueryRunning: boolean;
  readonly isAnalyzing: boolean;
  readonly isFullScreen: boolean;
  /** Whether the node's data is stale (needs re-materialization) */
  readonly isStale: boolean;
  readonly onFullScreenToggle: () => void;
  readonly onExecute: () => void;
  readonly onExportToTimeline?: () => void;
  readonly onchange?: () => void;
  readonly onFilterAdd?: (
    filter: UIFilter | UIFilter[],
    filterOperator?: 'AND' | 'OR',
  ) => void;
  readonly onColumnAdd?: (column: Column) => void;
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

    // Only show "Run Query" button when autoExecute is off AND node is stale
    const runButton =
      !autoExecute &&
      attrs.isStale &&
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

    // Menu items need to fetch table name asynchronously from TP.
    // Check if we have a response ready (not running, not analyzing, not stale).
    // The isStale check prevents exporting outdated data when the node's query
    // has changed but not yet been re-executed.
    const hasResponseReady =
      attrs.response &&
      !attrs.isQueryRunning &&
      !attrs.isAnalyzing &&
      !attrs.isStale;

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
          disabled: !(attrs.onExportToTimeline && hasResponseReady),
        }),
        m(MenuItem, {
          label: 'Copy Materialized Table Name',
          icon: 'content_copy',
          onclick: async () => {
            const tableName = await attrs.queryExecutionService.getTableName(
              attrs.node.nodeId,
            );
            if (tableName) {
              navigator.clipboard.writeText(tableName);
            }
          },
          title: 'Copy the materialized table name to clipboard',
          disabled: !hasResponseReady,
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
      // Get the SQL that caused the error (query is preserved during error)
      const failingSql = isAQuery(attrs.query) ? attrs.query.sql : undefined;

      return m(
        DataExplorerEmptyState,
        {
          icon: 'warning',
          variant: 'warning',
          title: attrs.node.state.issues.executionError.message,
        },
        [
          // Show the failing SQL if available
          failingSql &&
            m('.pf-failing-sql', [
              m('.pf-failing-sql__header', [
                m('.pf-failing-sql__label', 'Failed SQL:'),
                m(Button, {
                  icon: 'content_copy',
                  compact: true,
                  title: 'Copy SQL to clipboard',
                  onclick: () => {
                    navigator.clipboard.writeText(failingSql);
                  },
                }),
              ]),
              m('pre.pf-failing-sql__code', failingSql),
            ]),
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
        ],
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
      const schema: SchemaRegistry = {data: columnSchema};

      // Get sqlModules from attrs (centralized, not from node state)
      const {sqlModules} = attrs;

      // Capture columns for use in closures
      const responseColumns = attrs.response.columns;

      for (const c of responseColumns) {
        let cellRenderer: CellRenderer | undefined;
        let columnType: ColumnType | undefined;

        // Get column type information from the node
        const columnInfo = getColumnInfo(attrs.node, c);
        if (columnInfo) {
          // Set columnType based on the SQL type
          if (columnInfo.column.type) {
            columnType = getColumnType(columnInfo.column.type);
          }

          // Check if this is a timestamp column
          if (columnInfo.type === 'TIMESTAMP') {
            cellRenderer = createTimestampCellRenderer(attrs.trace);
          }
          // Check if this is a duration column
          else if (columnInfo.type === 'DURATION') {
            cellRenderer = createDurationCellRenderer(attrs.trace);
          }
        }

        columnSchema[c] = {cellRenderer, columnType};
      }

      // Build menu items for joinid columns (add columns from related tables)
      // Get existing column names from the node's schema for filtering
      const existingColumnNames = new Set(
        attrs.node.finalCols.map((col) => col.name),
      );

      const buildJoinidMenuItems = (): m.Children => {
        if (sqlModules === undefined) return undefined;

        // Group joinid columns by target table name
        const tableToJoinidColumns = new Map<
          string,
          Array<{joinidColumn: string; targetTable: string}>
        >();

        for (const c of responseColumns) {
          const columnInfo = getColumnInfo(attrs.node, c);
          if (columnInfo?.column.type?.kind === 'joinid') {
            const targetTableName = columnInfo.column.type.source.table;
            if (!tableToJoinidColumns.has(targetTableName)) {
              tableToJoinidColumns.set(targetTableName, []);
            }
            tableToJoinidColumns.get(targetTableName)!.push({
              joinidColumn: c,
              targetTable: targetTableName,
            });
          }
        }

        const tableSubmenus: m.Children[] = [];

        // Build submenus for each target table
        for (const [tableName, joinidColumns] of tableToJoinidColumns) {
          const targetTable = sqlModules.getTable(tableName);
          if (targetTable === undefined) continue;

          // Helper to build column menu items for a specific joinid column
          const buildColumnItems = (joinidColumn: string): m.Children[] => {
            return targetTable.columns.map((col) => {
              const field = `${joinidColumn}.${col.name}`;
              const isDisabled = existingColumnNames.has(col.name);
              return m(MenuItem, {
                label: col.name,
                disabled: isDisabled,
                onclick: isDisabled
                  ? undefined
                  : () => {
                      attrs.onColumnAdd?.({
                        id: field,
                        field,
                      });
                    },
              });
            });
          };

          if (joinidColumns.length === 1) {
            // Single joinid column - show columns directly under "From {table}"
            tableSubmenus.push(
              m(
                MenuItem,
                {label: `From ${tableName}`},
                buildColumnItems(joinidColumns[0].joinidColumn),
              ),
            );
          } else {
            // Multiple joinid columns - show "via [column]" submenus under "From {table}"
            const viaSubmenus = joinidColumns.map(({joinidColumn}) =>
              m(
                MenuItem,
                {label: `via ${joinidColumn}`},
                buildColumnItems(joinidColumn),
              ),
            );
            tableSubmenus.push(
              m(MenuItem, {label: `From ${tableName}`}, viaSubmenus),
            );
          }
        }

        // Wrap all table submenus under a single "Add columns" menu item
        if (tableSubmenus.length === 0) {
          return undefined;
        }

        return m(
          MenuItem,
          {label: 'Add column', icon: Icons.AddColumnRight},
          tableSubmenus,
        );
      };

      return [
        warning,
        m(DataGrid, {
          schema,
          rootSchema: 'data',
          columns: attrs.response.columns.map((col) => ({
            id: col,
            field: col,
          })),
          fillHeight: true,
          data: attrs.dataSource,
          enablePivotControls: false,
          structuredQueryCompatMode: true,
          canAddColumns: false,
          canRemoveColumns: false,
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
          addColumnMenuItems: buildJoinidMenuItems,
        }),
      ];
    }

    // Show spinner when the service is busy executing another node's query
    // and this node doesn't have a response yet (queued state).
    const isServiceBusy = attrs.queryExecutionService.isQueryExecuting();
    if (isServiceBusy && !attrs.response && !attrs.isQueryRunning) {
      return m(DataExplorerEmptyState, {}, [
        m('span.status-indicator', 'Queued...'),
        m(Spinner, {easing: true}),
      ]);
    }

    // Show a prominent execute button when autoExecute is false and node is stale
    const autoExecute = attrs.node.state.autoExecute ?? true;
    if (
      !autoExecute &&
      attrs.isStale &&
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

    // Show spinner when analyzing or when no response is available yet
    // (for autoExecute=true nodes that haven't run yet)
    if (!attrs.response || attrs.isAnalyzing) {
      return m(DataExplorerEmptyState, {}, m(Spinner, {easing: true}));
    }

    return null;
  }
}
