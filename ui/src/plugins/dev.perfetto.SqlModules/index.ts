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
import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {SqlModules, isTableEffectivelyDisabled} from './sql_modules';
import {extensions} from '../../components/extensions';
import {
  STDLIB_METADATA_SCHEMA,
  StdlibMetadata,
  loadSqlModulesFromTp,
} from './sql_modules_from_tp';

// Metadata JSON is small and static — fetch it once when the app starts so
// it is ready by the time a trace loads.
let metadataPromise: Promise<StdlibMetadata> | undefined;

function getMetadata(): Promise<StdlibMetadata> {
  if (metadataPromise === undefined) {
    metadataPromise = fetch(assetSrc('stdlib_docs.json'))
      .then((r) => r.json())
      .then((json) => STDLIB_METADATA_SCHEMA.parse(json));
  }
  return metadataPromise;
}

export default class SqlModulesPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.SqlModules';

  private sqlModules: SqlModules | undefined;

  static onActivate(_: App): void {
    // Kick off the metadata fetch early so it is ready before trace load.
    getMetadata();
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    const metadata = await getMetadata();
    this.sqlModules = await loadSqlModulesFromTp(trace, metadata);
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

        const actualTableName = chosenTable.replace(' (no data)', '');
        const module = this.sqlModules.getModuleForTable(actualTableName);
        if (module === undefined) {
          return;
        }

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
