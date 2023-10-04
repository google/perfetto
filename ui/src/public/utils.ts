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

export function getTrackName(args: Partial<{
  name: string | null,
  utid: number,
  processName: string | null,
  pid: number | null,
  threadName: string | null,
  tid: number | null,
  upid: number | null,
  kind: string,
  threadTrack: boolean
}>) {
  const {
    name,
    upid,
    utid,
    processName,
    threadName,
    pid,
    tid,
    kind,
    threadTrack,
  } = args;

  const hasName = name !== undefined && name !== null && name !== '[NULL]';
  const hasUpid = upid !== undefined && upid !== null;
  const hasUtid = utid !== undefined && utid !== null;
  const hasProcessName = processName !== undefined && processName !== null;
  const hasThreadName = threadName !== undefined && threadName !== null;
  const hasTid = tid !== undefined && tid !== null;
  const hasPid = pid !== undefined && pid !== null;
  const hasKind = kind !== undefined;
  const isThreadTrack = threadTrack !== undefined && threadTrack;

  // If we don't have any useful information (better than
  // upid/utid) we show the track kind to help with tracking
  // down where this is coming from.
  const kindSuffix = hasKind ? ` (${kind})` : '';

  if (isThreadTrack && hasName && hasTid) {
    return `${name} (${tid})`;
  } else if (hasName) {
    return `${name}`;
  } else if (hasUpid && hasPid && hasProcessName) {
    return `${processName} ${pid}`;
  } else if (hasUpid && hasPid) {
    return `Process ${pid}`;
  } else if (hasThreadName && hasTid) {
    return `${threadName} ${tid}`;
  } else if (hasTid) {
    return `Thread ${tid}`;
  } else if (hasUpid) {
    return `upid: ${upid}${kindSuffix}`;
  } else if (hasUtid) {
    return `utid: ${utid}${kindSuffix}`;
  } else if (hasKind) {
    return `Unnamed ${kind}`;
  }
  return 'Unknown';
}
