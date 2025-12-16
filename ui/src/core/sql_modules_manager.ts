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
import {openTableExplorer} from '../components/table_explorer';
import {
  SQL_MODULES_DOCS_SCHEMA,
  SqlModulesDocsSchema,
  SqlModulesImpl,
} from './sql_modules_impl';
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
 * Manager for SQL modules.
 * Created per-trace to handle schema initialization.
 */
export class SqlModulesManager {
  private sqlModules: SqlModules | undefined;
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

    // Open the table using the standalone function
    openTableExplorer(this.trace, {tableName: actualTableName});
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
}
