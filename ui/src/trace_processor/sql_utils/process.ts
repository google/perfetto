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
import {Upid} from './core_types';

// TODO(altimin): We should consider implementing some form of cache rather than querying
// the data from trace processor each time.

export interface ProcessInfo {
  upid: Upid;
  pid?: number;
  name?: string;
  uid?: number;
  packageName?: string;
  versionCode?: number;
}

export async function getProcessInfo(
  engine: Engine,
  upid: Upid,
): Promise<ProcessInfo> {
  const res = await engine.query(`
    include perfetto module android.process_metadata;
    select
      p.upid,
      p.pid,
      p.name,
      p.uid,
      m.package_name as packageName,
      m.version_code as versionCode
    from process p
    left join android_process_metadata m using (upid)
    where upid = ${upid};
  `);
  const row = res.firstRow({
    upid: NUM,
    pid: NUM,
    name: STR_NULL,
    uid: NUM_NULL,
    packageName: STR_NULL,
    versionCode: NUM_NULL,
  });
  return {
    upid,
    pid: row.pid,
    name: row.name ?? undefined,
    uid: fromNumNull(row.uid),
    packageName: row.packageName ?? undefined,
    versionCode: fromNumNull(row.versionCode),
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

export function getProcessName(info?: {
  name?: string;
  pid?: number;
}): string | undefined {
  return getDisplayName(info?.name, info?.pid);
}
