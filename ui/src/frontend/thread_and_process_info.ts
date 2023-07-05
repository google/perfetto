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

import {EngineProxy} from '../common/engine';
import {NUM, NUM_NULL, STR, STR_NULL} from '../common/query_result';
import {Upid, Utid} from './sql_types';
import {fromNumNull} from './sql_utils';

// Interface definitions for process and thread-related information
// and functions to extract them from SQL.

// TODO(altimin): Current implementation ends up querying process and thread
// information separately for each thread. Given that there is a limited
// numer of threads and processes, it might be easier to fetch this information
// once when loading the trace and then just look it up synchronously.

export interface ProcessInfo {
  upid: Upid;
  pid?: number;
  name?: string;
  uid?: number;
  packageName?: string;
  versionCode?: number;
}

export async function getProcessInfo(
    engine: EngineProxy, upid: Upid): Promise<ProcessInfo> {
  const it = (await engine.query(`
              SELECT pid, name, uid FROM process WHERE upid = ${upid};
            `)).iter({pid: NUM, name: STR_NULL, uid: NUM_NULL});
  if (!it.valid()) {
    return {upid};
  }
  const result: ProcessInfo = {
    upid,
    pid: it.pid,
    name: it.name || undefined,
  };

  if (it.pid === null) {
    return result;
  }
  result.pid = it.pid || undefined;

  if (it.uid === undefined) {
    return result;
  }

  const packageResult = await engine.query(`
                SELECT
                  package_name as packageName,
                  version_code as versionCode
                FROM package_list WHERE uid = ${it.uid};
              `);
  // The package_list table is not populated in some traces so we need to
  // check if the result has returned any rows.
  if (packageResult.numRows() > 0) {
    const packageDetails = packageResult.firstRow({
      packageName: STR,
      versionCode: NUM,
    });
    result.packageName = packageDetails.packageName;
    result.versionCode = packageDetails.versionCode || undefined;
  }
  return result;
}

function getDisplayName(name: string|undefined, id: number|undefined): string|
    undefined {
  if (name === undefined) {
    return id === undefined ? undefined : `${id}`;
  }
  return id === undefined ? name : `${name} [${id}]`;
}

export function getProcessName(info?: ProcessInfo): string|undefined {
  return getDisplayName(info?.name, info?.pid);
}

export interface ThreadInfo {
  utid: Utid;
  tid?: number;
  name?: string;
  process?: ProcessInfo;
}

export async function getThreadInfo(
    engine: EngineProxy, utid: Utid): Promise<ThreadInfo> {
  const it = (await engine.query(`
        SELECT tid, name, upid
        FROM thread
        WHERE utid = ${utid};
    `)).iter({tid: NUM, name: STR_NULL, upid: NUM_NULL});
  if (!it.valid()) {
    return {
      utid,
    };
  }
  const upid = fromNumNull(it.upid) as (Upid | undefined);
  return {
    utid,
    tid: it.tid,
    name: it.name || undefined,
    process: upid ? await getProcessInfo(engine, upid) : undefined,
  };
}

export function getThreadName(info?: ThreadInfo): string|undefined {
  return getDisplayName(info?.name, info?.tid);
}

// Return the full thread name, including the process name.
export function getFullThreadName(info?: ThreadInfo): string|undefined {
  if (info?.process === undefined) {
    return getThreadName(info);
  }
  return `${getThreadName(info)} ${getProcessName(info.process)}`;
}
