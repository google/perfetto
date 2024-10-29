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

import {sqlTableRegistry} from '../../frontend/widgets/sql/table/sql_table_registry';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {getProcessTable} from './table';
import {extensions} from '../../public/lib/extensions';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Process';
  async onTraceLoad(ctx: Trace) {
    sqlTableRegistry['process'] = getProcessTable();
    ctx.commands.registerCommand({
      id: 'perfetto.ShowTable.process',
      name: 'Open table: process',
      callback: () => {
        extensions.addSqlTableTab(ctx, {
          table: getProcessTable(),
        });
      },
    });
  }
}
