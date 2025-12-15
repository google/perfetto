// Copyright (C) 2024 The Android Open Source Project
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
import {assetSrc} from '../../base/assets';
import {defer} from '../../base/deferred';
import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {Tab} from '../../public/tab';
import {SqlModules} from './sql_modules';
import {
  SQL_MODULES_DOCS_SCHEMA,
  SqlModulesDocsSchema,
  SqlModulesImpl,
} from './sql_modules_impl';
import {
  DataGrid,
  DataGridAttrs,
} from '../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {addEphemeralTab} from '../../components/details/add_ephemeral_tab';
import {DetailsShell} from '../../widgets/details_shell';
import {sqlTablesToSchemas} from '../../components/widgets/datagrid/sql_table_converter';
import {SQLSchemaRegistry} from '../../components/widgets/datagrid/sql_schema';
import {SqlTable} from './sql_modules';
import {Filter} from '../../components/widgets/datagrid/model';
import {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';

const docs = defer<SqlModulesDocsSchema>();

export default class SqlModulesPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.SqlModules';

  private sqlModules: SqlModules | undefined;
  private trace?: Trace;
  // Cached unified schema
  private sqlSchema?: SQLSchemaRegistry;
  private displaySchema?: SchemaRegistry;

  static onActivate(_: App): void {
    // Load the SQL modules JSON file when the plugin when the app starts up,
    // rather than waiting until trace load.
    loadJson().then(docs.resolve.bind(docs));
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    this.trace = trace;

    docs.then(async (resolvedDocs) => {
      const impl = new SqlModulesImpl(trace, resolvedDocs);
      await impl.waitForInit();
      this.sqlModules = impl;

      const tables = impl.listTables();
      const {sqlSchema, displaySchema} = sqlTablesToSchemas(tables, trace);
      this.sqlSchema = sqlSchema;
      this.displaySchema = displaySchema;

      m.redraw();
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.OpenSqlModulesTable',
      name: 'Open table...',
      callback: () => this.promptAndOpenTable(),
    });
  }

  private async promptAndOpenTable(): Promise<void> {
    if (!this.sqlModules || !this.trace) {
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

    // Open the table using the new DataGrid-based viewer
    this.openTableExplorer(actualTableName);
  }

  /**
   * Opens a table in a new tab using DataGrid with full schema support.
   *
   * @param tableName The name of the table to open
   * @param options Configuration options
   * @param options.filters Initial filters to apply
   * @param options.initialColumns Initial columns to display
   * @param options.customTables Custom table definitions to inject into the schema
   *   for this invocation only. Useful for adding ad-hoc table relationships or
   *   overriding existing table definitions.
   */
  openTableExplorer(
    tableName: string,
    options?: {
      filters?: Filter[];
      initialColumns?: string[];
      customTables?: SqlTable[];
    },
  ): void {
    if (!this.sqlModules || !this.trace) {
      throw new Error('SqlModules not initialized');
    }

    // Get base schemas
    const baseSqlSchema = this.sqlSchema;
    const baseDisplaySchema = this.displaySchema;

    if (baseSqlSchema === undefined || baseDisplaySchema === undefined) {
      throw new Error('Schemas not initialized');
    }

    // Determine which schemas to use
    let sqlSchema: SQLSchemaRegistry;
    let displaySchema: SchemaRegistry;

    if (options?.customTables && options.customTables.length > 0) {
      // Convert custom tables to schemas
      const customSchemas = sqlTablesToSchemas(
        options.customTables,
        this.trace,
      );

      // Merge custom schemas with base schemas
      // Custom tables override base tables with the same name
      sqlSchema = {...baseSqlSchema, ...customSchemas.sqlSchema};
      displaySchema = {...baseDisplaySchema, ...customSchemas.displaySchema};
    } else {
      // Use cached schemas as-is
      sqlSchema = baseSqlSchema;
      displaySchema = baseDisplaySchema;
    }

    // Check if table exists in the merged schema
    const table = this.sqlModules.getTable(tableName);
    const customTable = options?.customTables?.find(
      (t) => t.name === tableName,
    );

    if (!table && !customTable) {
      throw new Error(`Table not found: ${tableName}`);
    }

    // Get the module for INCLUDE statement (from base table if available)
    const module = this.sqlModules.getModuleForTable(tableName);
    const includeKey = module?.includeKey;

    // Create datasource with (potentially merged) schema
    const dataSource = new SQLDataSource({
      engine: this.trace.engine,
      sqlSchema,
      rootSchemaName: tableName,
      prelude: includeKey
        ? `INCLUDE PERFETTO MODULE ${includeKey};`
        : undefined,
    });

    // Create and open tab
    addEphemeralTab(
      this.trace,
      'sqlTable',
      new DataGridSqlTableTab({
        displayName: tableName,
        dataSource,
        schema: displaySchema,
        rootSchema: tableName,
        initialFilters: options?.filters,
        initialColumns: options?.initialColumns,
      }),
    );
  }

  getSqlModules(): SqlModules | undefined {
    return this.sqlModules;
  }
}

/**
 * Tab implementation for DataGrid-based SQL table viewer
 */
class DataGridSqlTableTab implements Tab {
  private readonly displayName: string;
  private readonly dataSource: SQLDataSource;
  private readonly schema: SchemaRegistry;
  private readonly rootSchema: string;
  private readonly initialFilters?: Filter[];
  private readonly initialColumns?: string[];

  constructor(config: {
    displayName: string;
    dataSource: SQLDataSource;
    schema: SchemaRegistry;
    rootSchema: string;
    initialFilters?: Filter[];
    initialColumns?: string[];
  }) {
    this.displayName = config.displayName;
    this.dataSource = config.dataSource;
    this.schema = config.schema;
    this.rootSchema = config.rootSchema;
    this.initialFilters = config.initialFilters;
    this.initialColumns = config.initialColumns;
  }

  getTitle(): string {
    return `Table: ${this.displayName}`;
  }

  render(): m.Children {
    return m(
      DetailsShell,
      {
        title: 'Table',
        description: this.displayName,
        fillHeight: true,
      },
      m(DataGrid, {
        schema: this.schema,
        rootSchema: this.rootSchema,
        data: this.dataSource,
        initialFilters: this.initialFilters,
        initialColumns: this.initialColumns?.map((colName) => ({
          field: colName,
        })),
        fillHeight: true,
        showExportButton: true,
      } satisfies DataGridAttrs),
    );
  }
}

async function loadJson() {
  const x = await fetch(assetSrc('stdlib_docs.json'));
  const json = await x.json();
  return SQL_MODULES_DOCS_SCHEMA.parse(json);
}
