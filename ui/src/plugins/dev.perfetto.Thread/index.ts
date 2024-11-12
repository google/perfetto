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
import {getThreadTable} from './table';
import {extensions} from '../../public/lib/extensions';
import {ThreadDesc, ThreadMap} from '../dev.perfetto.Thread/threads';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import {assertExists} from '../../base/logging';

async function listThreads(trace: Trace) {
  const query = `select
        utid,
        tid,
        pid,
        ifnull(thread.name, '') as threadName,
        ifnull(
          case when length(process.name) > 0 then process.name else null end,
          thread.name) as procName,
        process.cmdline as cmdline
        from (select * from thread order by upid) as thread
        left join (select * from process order by upid) as process
        using(upid)`;
  const result = await trace.engine.query(query);
  const threads = new Map<number, ThreadDesc>();
  const it = result.iter({
    utid: NUM,
    tid: NUM,
    pid: NUM_NULL,
    threadName: STR,
    procName: STR_NULL,
    cmdline: STR_NULL,
  });
  for (; it.valid(); it.next()) {
    const utid = it.utid;
    const tid = it.tid;
    const pid = it.pid === null ? undefined : it.pid;
    const threadName = it.threadName;
    const procName = it.procName === null ? undefined : it.procName;
    const cmdline = it.cmdline === null ? undefined : it.cmdline;
    threads.set(utid, {utid, tid, threadName, pid, procName, cmdline});
  }
  return threads;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Thread';
  private threads?: ThreadMap;

  async onTraceLoad(ctx: Trace) {
    sqlTableRegistry['thread'] = getThreadTable();
    ctx.commands.registerCommand({
      id: 'perfetto.ShowTable.thread',
      name: 'Open table: thread',
      callback: () => {
        extensions.addSqlTableTab(ctx, {
          table: getThreadTable(),
        });
      },
    });
    this.threads = await listThreads(ctx);
  }

  getThreadMap() {
    return assertExists(this.threads);
  }
}
