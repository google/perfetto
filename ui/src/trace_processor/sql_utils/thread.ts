// Copyright (C) 2023 The Android Open Source Project
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

import {Engine} from '../engine';
import {NUM, NUM_NULL, STR_NULL} from '../query_result';
import {fromNumNull} from '../sql_utils';
import {ProcessInfo, getProcessInfo, getProcessName} from './process';
import {Upid, Utid} from './core_types';

// TODO(altimin): We should consider implementing some form of cache rather than querying
// the data from trace processor each time.

export interface ThreadInfo {
  utid: Utid;
  tid?: number;
  name?: string;
  process?: ProcessInfo;
}

export async function getThreadInfo(
  engine: Engine,
  utid: Utid,
): Promise<ThreadInfo> {
  const it = (
    await engine.query(`
        SELECT tid, name, upid
        FROM thread
        WHERE utid = ${utid};
    `)
  ).iter({tid: NUM, name: STR_NULL, upid: NUM_NULL});
  if (!it.valid()) {
    return {
      utid,
    };
  }
  const upid = fromNumNull(it.upid) as Upid | undefined;
  return {
    utid,
    tid: it.tid,
    name: it.name ?? undefined,
    process: upid ? await getProcessInfo(engine, upid) : undefined,
  };
}

function getDisplayName(
  name: string | undefined,
  id: number | undefined,
): string | undefined {
  if (name === undefined) {
    return id === undefined ? undefined : `${id}`;
  }
  return id === undefined ? name : `${name} [${id}]`;
}

export function getThreadName(info?: {
  name?: string;
  tid?: number;
}): string | undefined {
  return getDisplayName(info?.name, info?.tid);
}

// Return the full thread name, including the process name.
export function getFullThreadName(info?: ThreadInfo): string | undefined {
  if (info?.process === undefined) {
    return getThreadName(info);
  }
  return `${getThreadName(info)} ${getProcessName(info.process)}`;
}
