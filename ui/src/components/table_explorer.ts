import m from 'mithril';
import {Tab} from '../public/tab';
import {
  DataGrid,
  DataGridApi,
  DataGridAttrs,
} from './widgets/datagrid/datagrid';
import {SQLDataSource} from './widgets/datagrid/sql_data_source';
import {DetailsShell} from '../widgets/details_shell';
import {Filter, Column, Pivot} from './widgets/datagrid/model';
import {
  getDefaultVisibleFields,
  SchemaRegistry,
} from './widgets/datagrid/datagrid_schema';
import {DataGridExportButton} from './widgets/datagrid/export_button';
import {AddDebugTrackMenu} from './tracks/add_debug_track_menu';
import {Button} from '../widgets/button';
import {PopupMenu} from '../widgets/menu';
import {PopupPosition} from '../widgets/popup';
import {Trace} from '../public/trace';
import {SqlTable} from '../public/sql_modules';
import {sqlTablesToSchemas} from './sql_table_converter';
import {addEphemeralTab} from './details/add_ephemeral_tab';

export interface TableExplorerConfig {
  trace: Trace;
  displayName: string;
  dataSource: SQLDataSource;
  schema: SchemaRegistry;
  rootSchema: string;
  initialFilters?: Filter[];
  initialColumns?: Column[];
  initialPivot?: Pivot;
  onDuplicate?: (state: TableExplorerState) => void;
}

export interface TableExplorerState {
  filters: readonly Filter[];
  columns: readonly Column[];
  pivot?: Pivot;
}

// Wraps a DataGrid in a Details shell for use inside tab.
export class TableExplorer implements Tab {
  private readonly trace: Trace;
  private readonly displayName: string;
  private readonly dataSource: SQLDataSource;
  private readonly schema: SchemaRegistry;
  private readonly rootSchema: string;
  private readonly onDuplicate?: (state: TableExplorerState) => void;
  private gridApi?: DataGridApi;
  private columns: readonly Column[] = [];
  private filters: readonly Filter[] = [];
  private pivot?: Pivot;

  constructor(config: TableExplorerConfig) {
    this.trace = config.trace;
    this.displayName = config.displayName;
    this.dataSource = config.dataSource;
    this.schema = config.schema;
    this.rootSchema = config.rootSchema;
    this.onDuplicate = config.onDuplicate;
    this.columns =
      config.initialColumns ??
      getDefaultVisibleFields(this.schema, this.rootSchema).map((col) => {
        return {field: col};
      });
    this.filters = config.initialFilters ?? [];
    this.pivot = config.initialPivot;
  }

  getTitle(): string {
    return `Table: ${this.displayName}`;
  }

  render(): m.Children {
    const rowCount = this.gridApi?.getRowCount();
    const rowCountText =
      rowCount !== undefined ? `${rowCount.toLocaleString()} rows` : '';

    // Get the current query from the datasource
    const currentQuery = this.dataSource.getCurrentQuery();

    return m(
      DetailsShell,
      {
        title: 'Table',
        description: this.displayName,
        fillHeight: true,
        buttons: [
          rowCountText && m('span.pf-table-explorer__row-count', rowCountText),
          currentQuery &&
            m(
              PopupMenu,
              {
                trigger: m(Button, {
                  label: 'Add debug track',
                }),
                position: PopupPosition.Top,
              },
              m(AddDebugTrackMenu, {
                trace: this.trace,
                query: currentQuery,
                availableColumns: this.columns.map((col) => col.field),
              }),
            ),
          m(DataGridExportButton, {
            onExportData: (format) => this.gridApi!.exportData(format),
          }),
          this.onDuplicate &&
            m(Button, {
              label: 'Duplicate tab',
              icon: 'tab_duplicate',
              onclick: () => {
                this.onDuplicate?.({
                  filters: this.filters,
                  columns: this.columns,
                  pivot: this.pivot,
                });
              },
            }),
        ],
      },
      m(DataGrid, {
        schema: this.schema,
        rootSchema: this.rootSchema,
        data: this.dataSource,
        filters: this.filters,
        pivot: this.pivot,
        fillHeight: true,
        columns: this.columns,
        onReady: (api) => {
          this.gridApi = api;
        },
        onColumnsChanged: (columns) => {
          this.columns = columns;
        },
        onFiltersChanged: (filters) => {
          this.filters = filters;
        },
        onPivotChanged: (pivot) => {
          this.pivot = pivot;
        },
      } satisfies DataGridAttrs),
    );
  }
}

export interface OpenTableExplorerConfig {
  tableName: string;
  initialFilters?: Filter[];
  initialColumns?: Column[];
  initialPivot?: Pivot;
  // Custom table definitions to inject into the schema for this invocation
  // only. Useful for adding ad-hoc table relationships or overriding existing
  // table definitions.
  customTables?: SqlTable[];
  // SQL statements to execute before the main query. Typically used for
  // INCLUDE statements.
  preamble?: string;
}

/**
 * Opens a table in a new tab using DataGrid with full schema support.
 * This is a standalone function that takes a trace object as a parameter.
 */
export function openTableExplorer(
  trace: Trace,
  config: OpenTableExplorerConfig,
): void {
  const {
    tableName,
    initialFilters,
    initialColumns,
    initialPivot,
    customTables,
  } = config;

  const sqlModules = trace.getSqlModules();
  if (!sqlModules) {
    throw new Error('SqlModules not initialized');
  }

  // Get all tables and convert to schemas
  const allTables = sqlModules.listTables();
  const {sqlSchema: baseSqlSchema, displaySchema: baseDisplaySchema} =
    sqlTablesToSchemas(allTables, trace);

  // Determine which schemas to use
  let sqlSchema = baseSqlSchema;
  let displaySchema = baseDisplaySchema;

  if (customTables && customTables.length > 0) {
    // Convert custom tables to schemas and merge with base schemas
    const customSchemas = sqlTablesToSchemas(customTables, trace);
    sqlSchema = {...baseSqlSchema, ...customSchemas.sqlSchema};
    displaySchema = {...baseDisplaySchema, ...customSchemas.displaySchema};
  }

  // Check if table exists in the merged schema
  const table = sqlModules.getTable(tableName);
  const customTable = customTables?.find((t) => t.name === tableName);
  if (!table && !customTable) {
    throw new Error(`Table not found: ${tableName}`);
  }

  // Build preamble from config or module include
  let preamble: string | undefined;
  if (config.preamble) {
    preamble = config.preamble;
  } else {
    const module = sqlModules.getModuleForTable(tableName);
    if (module?.includeKey) {
      preamble = `INCLUDE PERFETTO MODULE ${module.includeKey};`;
    }
  }

  // Create datasource with (potentially merged) schema
  const dataSource = new SQLDataSource({
    engine: trace.engine,
    sqlSchema,
    rootSchemaName: tableName,
    preamble,
  });

  // Determine columns to use
  const columns =
    initialColumns ??
    getDefaultVisibleFields(displaySchema, tableName).map((col) => ({
      field: col,
    }));

  // Create and open tab
  addEphemeralTab(
    trace,
    'tableExplorer',
    new TableExplorer({
      trace,
      displayName: tableName,
      dataSource,
      schema: displaySchema,
      rootSchema: tableName,
      initialFilters,
      initialColumns: columns,
      initialPivot,
      onDuplicate: (state) => {
        openTableExplorer(trace, {
          ...config,
          initialFilters: [...state.filters],
          initialColumns: [...state.columns],
          initialPivot: state.pivot,
        });
      },
    }),
  );
}
