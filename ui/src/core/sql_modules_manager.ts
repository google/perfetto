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

import {assetSrc} from '../base/assets';
import {defer, Deferred} from '../base/deferred';
import {Trace} from '../public/trace';
import {Column, Filter, Pivot, SqlTable} from '../public/table';
import {SQLDataSource} from '../components/widgets/datagrid/sql_data_source';
import {addEphemeralTab} from '../components/details/add_ephemeral_tab';
import {TableExplorer} from '../components/table_explorer';
import {
  SchemaRegistry,
  getDefaultVisibleFields,
} from '../components/widgets/datagrid/datagrid_schema';
import {SQLSchemaRegistry} from '../components/widgets/datagrid/sql_schema';
import {
  SQL_MODULES_DOCS_SCHEMA,
  SqlModulesDocsSchema,
  SqlModulesImpl,
} from './sql_modules_impl';
import {sqlTablesToSchemas} from './sql_table_converter';
import {SqlModules} from '../public/sql_modules';

// Deferred JSON loading - starts when initSqlModulesLoader() is called
let docsDeferred: Deferred<SqlModulesDocsSchema> | undefined;

/**
 * Initialize the SQL modules loader. Should be called during app startup
 * to begin loading the stdlib_docs.json file in the background.
 */
export function initSqlModulesLoader(): void {
  if (docsDeferred !== undefined) {
    return; // Already initialized
  }
  docsDeferred = defer<SqlModulesDocsSchema>();
  loadJson().then(docsDeferred.resolve.bind(docsDeferred));
}

async function loadJson(): Promise<SqlModulesDocsSchema> {
  const response = await fetch(assetSrc('stdlib_docs.json'));
  const json = await response.json();
  return SQL_MODULES_DOCS_SCHEMA.parse(json);
}

/**
 * Manager for SQL modules and table exploration.
 * Created per-trace to handle schema initialization and provide openTableExplorer.
 */
export class SqlModulesManager {
  private sqlModules: SqlModules | undefined;
  private sqlSchema: SQLSchemaRegistry | undefined;
  private displaySchema: SchemaRegistry | undefined;
  private initPromise: Promise<void>;

  constructor(private readonly trace: Trace) {
    this.initPromise = this.initialize();
  }

  private async initialize(): Promise<void> {
    if (docsDeferred === undefined) {
      throw new Error(
        'SqlModulesManager: initSqlModulesLoader() was not called',
      );
    }

    const docs = await docsDeferred;
    const impl = new SqlModulesImpl(this.trace, docs);
    await impl.waitForInit();
    this.sqlModules = impl;

    const tables = impl.listTables();
    const {sqlSchema, displaySchema} = sqlTablesToSchemas(tables, this.trace);
    this.sqlSchema = sqlSchema;
    this.displaySchema = displaySchema;

    // Register the "Open table..." command
    this.trace.commands.registerCommand({
      id: 'dev.perfetto.OpenSqlModulesTable',
      name: 'Open table...',
      callback: () => this.promptAndOpenTable(),
    });
  }

  private async promptAndOpenTable(): Promise<void> {
    if (!this.sqlModules) {
      window.alert('Sql modules are still loading... Please wait.');
      return;
    }

    const tables = this.sqlModules.listTablesNames();

    // Annotate disabled modules in the prompt
    const annotatedTables = tables.map((tableName) => {
      const perfettoModule = this.sqlModules!.getModuleForTable(tableName);
      if (
        perfettoModule &&
        this.sqlModules!.isModuleDisabled(perfettoModule.includeKey)
      ) {
        return `${tableName} (no data)`;
      }
      return tableName;
    });

    const chosenTable = await this.trace.omnibox.prompt(
      'Choose a table...',
      annotatedTables,
    );
    if (chosenTable === undefined) {
      return;
    }

    // Strip the annotation if present
    const actualTableName = chosenTable.replace(' (no data)', '');
    const perfettoModule = this.sqlModules.getModuleForTable(actualTableName);
    if (perfettoModule === undefined) {
      return;
    }

    // Warn if opening a disabled module
    if (this.sqlModules.isModuleDisabled(perfettoModule.includeKey)) {
      const proceed = window.confirm(
        `Warning: The module "${perfettoModule.includeKey}" may not have data in this trace. ` +
          `The table might be empty. Continue anyway?`,
      );
      if (!proceed) {
        return;
      }
    }

    // Open the table
    this.openTableExplorer({tableName: actualTableName});
  }

  /**
   * Wait for the SQL modules to be fully initialized.
   */
  async waitForInit(): Promise<void> {
    await this.initPromise;
  }

  /**
   * Get the underlying SqlModules instance.
   * Returns undefined if not yet initialized.
   */
  getSqlModules(): SqlModules | undefined {
    return this.sqlModules;
  }

  /**
   * Opens a table in a new tab using DataGrid with full schema support.
   */
  openTableExplorer(config: {
    tableName: string;
    initialFilters?: Filter[];
    initialColumns?: Column[];
    initialPivot?: Pivot;
    customTables?: SqlTable[];
    preamble?: string;
  }): void {
    const {
      tableName,
      initialFilters,
      initialColumns,
      initialPivot,
      customTables,
    } = config;

    if (!this.sqlModules || !this.sqlSchema || !this.displaySchema) {
      throw new Error('SqlModules not initialized');
    }

    // Determine which schemas to use
    let sqlSchema: SQLSchemaRegistry;
    let displaySchema: SchemaRegistry;

    if (customTables && customTables.length > 0) {
      // Convert custom tables to schemas and merge with base schemas
      const customSchemas = sqlTablesToSchemas(customTables, this.trace);
      sqlSchema = {...this.sqlSchema, ...customSchemas.sqlSchema};
      displaySchema = {...this.displaySchema, ...customSchemas.displaySchema};
    } else {
      sqlSchema = this.sqlSchema;
      displaySchema = this.displaySchema;
    }

    // Check if table exists in the merged schema
    const table = this.sqlModules.getTable(tableName);
    const customTable = customTables?.find((t) => t.name === tableName);
    if (!table && !customTable) {
      throw new Error(`Table not found: ${tableName}`);
    }

    // Build preamble from config or module include
    let preamble: string | undefined;
    if (config.preamble) {
      preamble = config.preamble;
    } else {
      const module = this.sqlModules.getModuleForTable(tableName);
      if (module?.includeKey) {
        preamble = `INCLUDE PERFETTO MODULE ${module.includeKey};`;
      }
    }

    // Create datasource with (potentially merged) schema
    const dataSource = new SQLDataSource({
      engine: this.trace.engine,
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
      this.trace,
      'tableExplorer',
      new TableExplorer({
        trace: this.trace,
        displayName: tableName,
        dataSource,
        schema: displaySchema,
        rootSchema: tableName,
        initialFilters,
        initialColumns: columns,
        initialPivot,
        onDuplicate: (state) => {
          this.openTableExplorer({
            ...config,
            initialFilters: [...state.filters],
            initialColumns: [...state.columns],
            initialPivot: state.pivot,
          });
        },
      }),
    );
  }
}
