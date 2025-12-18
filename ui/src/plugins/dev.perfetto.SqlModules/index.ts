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
import {extensions} from '../../components/extensions';
import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {SqlModules} from './sql_modules';
import {
  SQL_MODULES_DOCS_SCHEMA,
  SqlModulesDocsSchema,
  SqlModulesImpl,
} from './sql_modules_impl';

const docs = defer<SqlModulesDocsSchema>();

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.SqlModules';

  private sqlModules: SqlModules | undefined;

  static onActivate(_: App): void {
    // Load the SQL modules JSON file when the plugin when the app starts up,
    // rather than waiting until trace load.
    loadJson().then(docs.resolve.bind(docs));
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    docs.then(async (resolvedDocs) => {
      const impl = new SqlModulesImpl(trace, resolvedDocs);
      impl.waitForInit().then(() => {
        this.sqlModules = impl;
        m.redraw();
      });
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.OpenSqlModulesTable',
      name: 'Open table...',
      callback: async () => {
        if (!this.sqlModules) {
          window.alert('Sql modules are still loading... Please wait.');
          return;
        }

        const tables = this.sqlModules.listTablesNames();

        // Annotate disabled modules in the prompt
        const annotatedTables = tables.map((tableName) => {
          const module = this.sqlModules!.getModuleForTable(tableName);
          if (module && this.sqlModules!.isModuleDisabled(module.includeKey)) {
            return `${tableName} (no data)`;
          }
          return tableName;
        });

        const chosenTable = await trace.omnibox.prompt(
          'Choose a table...',
          annotatedTables,
        );
        if (chosenTable === undefined) {
          return;
        }

        // Strip the annotation if present
        const actualTableName = chosenTable.replace(' (no data)', '');
        const module = this.sqlModules.getModuleForTable(actualTableName);
        if (module === undefined) {
          return;
        }

        // Warn if opening a disabled module
        if (this.sqlModules.isModuleDisabled(module.includeKey)) {
          const proceed = window.confirm(
            `Warning: The module "${module.includeKey}" may not have data in this trace. ` +
              `The table might be empty. Continue anyway?`,
          );
          if (!proceed) {
            return;
          }
        }

        const sqlTable = module.getSqlTableDefinition(actualTableName);
        sqlTable &&
          extensions.addLegacySqlTableTab(trace, {
            table: sqlTable,
          });
      },
    });
  }

  getSqlModules(): SqlModules | undefined {
    return this.sqlModules;
  }
}

async function loadJson() {
  const x = await fetch(assetSrc('stdlib_docs.json'));
  const json = await x.json();
  return SQL_MODULES_DOCS_SCHEMA.parse(json);
}
