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
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {type SqlModules, isTableEffectivelyDisabled} from './sql_modules';
import {extensions} from '../../components/extensions';
import {
  STDLIB_METADATA_SCHEMA,
  type StdlibMetadata,
  loadSqlModulesFromTp,
} from './sql_modules_from_tp';

const metadata = defer<StdlibMetadata>();

export default class SqlModulesPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.SqlModules';

  private sqlModules: SqlModules | undefined;

  static onActivate(_: App): void {
    // Eagerly start loading the metadata when the plugin starts up,
    // rather than waiting until trace load.
    loadMetadata().then(metadata.resolve.bind(metadata));
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    this.sqlModules = await loadSqlModulesFromTp(trace, await metadata);
    m.redraw();

    trace.commands.registerCommand({
      id: 'dev.perfetto.OpenSqlModulesTable',
      name: 'Open table...',
      callback: async () => {
        if (!this.sqlModules) {
          window.alert('Sql modules are still loading... Please wait.');
          return;
        }

        const tables = this.sqlModules.listTablesNames();

        // Annotate disabled tables in the prompt
        const annotatedTables = tables.map((tableName) => {
          if (isTableEffectivelyDisabled(this.sqlModules!, tableName)) {
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

        // Warn if opening a disabled table
        if (isTableEffectivelyDisabled(this.sqlModules, actualTableName)) {
          const proceed = window.confirm(
            `Warning: The table "${actualTableName}" may not have data in this trace. ` +
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

  ensureInitialized(): Promise<void> {
    if (this.sqlModules) {
      return this.sqlModules.ensureInitialized();
    }
    return Promise.resolve();
  }
}

async function loadMetadata() {
  const x = await fetch(assetSrc('stdlib_docs.json'));
  const json = await x.json();
  return STDLIB_METADATA_SCHEMA.parse(json);
}
