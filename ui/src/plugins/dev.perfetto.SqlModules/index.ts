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

import {assetSrc} from '../../base/assets';
import {assertExists} from '../../base/logging';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {SqlModules} from './sql_modules';
import {SQL_MODULES_DOCS_SCHEMA, SqlModulesImpl} from './sql_modules_impl';
import {extensions} from '../../components/extensions';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.SqlModules';
  private sqlModules?: SqlModules;

  async onTraceLoad(ctx: Trace) {
    const resp = await fetch(assetSrc('stdlib_docs.json'));
    const json = await resp.json();
    const docs = SQL_MODULES_DOCS_SCHEMA.parse(json);
    const sqlModules = new SqlModulesImpl(docs);
    this.sqlModules = sqlModules;

    ctx.commands.registerCommand({
      id: 'perfetto.OpenSqlModulesTable',
      name: 'Open table...',
      callback: async () => {
        const tables = sqlModules.listTables();
        const tableName = await ctx.omnibox.prompt('Choose a table...', tables);
        if (tableName === undefined) {
          return;
        }
        const module = sqlModules.getModuleForTable(tableName);
        if (module === undefined) {
          return;
        }
        const sqlTable = module.getSqlTableDescription(tableName);
        sqlTable &&
          extensions.addLegacySqlTableTab(ctx, {
            table: sqlTable,
          });
      },
    });
  }

  getSqlModules() {
    return assertExists(this.sqlModules);
  }
}
