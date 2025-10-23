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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {
  ProcessDesc,
  ThreadDesc,
  ThreadMap,
} from '../dev.perfetto.Thread/threads';
import {
  LONG,
  LONG_NULL,
  NUM,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {assertExists} from '../../base/logging';

async function listThreads(trace: Trace) {
  const query = `
    select
      utid,
      tid,
      pid,
      ifnull(thread.name, '') as threadName,
      ifnull(
        case when length(process.name) > 0 then process.name else null end,
        thread.name) as procName,
      process.cmdline as cmdline
    from (select * from thread order by upid) as thread
    left join (select * from process order by upid) as process using(upid)
  `;
  const result = await trace.engine.query(query);
  const threads = new Map<number, ThreadDesc>();
  const it = result.iter({
    utid: NUM,
    tid: LONG,
    pid: LONG_NULL,
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

async function listProcesses(trace: Trace) {
  const query = `
    select
      upid,
      pid,
      case
        when length(process.name) > 0 then process.name
        else null
      end as procName,
      cmdline
    from process
  `;
  const result = await trace.engine.query(query);
  const processMap = new Map<number, ProcessDesc>();
  const it = result.iter({
    upid: NUM,
    pid: LONG,
    procName: STR_NULL,
    cmdline: STR_NULL,
  });
  for (; it.valid(); it.next()) {
    processMap.set(it.upid, {
      upid: it.upid,
      pid: it.pid,
      procName: it.procName ?? undefined,
      cmdline: it.cmdline ?? undefined,
    });
  }
  return processMap;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Thread';
  static readonly description = `
    Extracts thread and process information from traces, making this information
    available to other plugins. Also adds track filtering criteria to allow
    track filtering by thread and process.
  `;
  private threads?: ThreadMap;

  async onTraceLoad(trace: Trace) {
    const threadMap = await listThreads(trace);
    const processMap = await listProcesses(trace);
    this.threads = threadMap;

    // Add a track filter criteria so that tracks may be filtered by process.
    trace.tracks.registerTrackFilterCriteria({
      name: 'Process',
      options: Array.from(processMap.entries()).map(([upid, process]) => {
        const procName = process.procName ?? '<no name>';
        return {
          key: upid.toString(),
          label: `[${upid}] ${procName}`,
        };
      }),
      predicate: (node, filterTerm) => {
        if (node.uri === undefined) return false;
        const track = trace.tracks.getTrack(node.uri);
        if (!track) return false;
        return track.tags?.upid === Number(filterTerm);
      },
    });

    // Add a track filter criteria so that tracks may be filtered by thread.
    trace.tracks.registerTrackFilterCriteria({
      name: 'Thread',
      options: Array.from(threadMap.entries()).map(([utid, thread]) => {
        const procName = thread.threadName ?? '<no name>';
        return {
          key: utid.toString(),
          label: `[${utid}] ${procName}`,
        };
      }),
      predicate: (node, filterTerm) => {
        if (node.uri === undefined) return false;
        const track = trace.tracks.getTrack(node.uri);
        if (!track) return false;
        return track.tags?.utid === Number(filterTerm);
      },
    });
  }

  getThreadMap() {
    return assertExists(this.threads);
  }
}
