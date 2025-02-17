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

import {TimeSpan} from '../base/time';
import {exists} from '../base/utils';
import {maybeMachineLabel} from '../base/multi_machine_trace';
import {Trace} from './trace';

export function getTrackName(
  args: Partial<{
    name: string | null;
    utid: number | null;
    processName: string | null;
    pid: number | null;
    threadName: string | null;
    tid: number | null;
    upid: number | null;
    userName: string | null;
    uid: number | null;
    kind: string;
    threadTrack: boolean;
    uidTrack: boolean;
    machine: number | null;
  }>,
) {
  const {
    name,
    upid,
    utid,
    processName,
    threadName,
    pid,
    tid,
    userName,
    uid,
    kind,
    threadTrack,
    uidTrack,
    machine,
  } = args;

  const hasName = name !== undefined && name !== null && name !== '[NULL]';
  const hasUpid = upid !== undefined && upid !== null;
  const hasUtid = utid !== undefined && utid !== null;
  const hasProcessName = processName !== undefined && processName !== null;
  const hasThreadName = threadName !== undefined && threadName !== null;
  const hasUserName = userName !== undefined && userName !== null;
  const hasTid = tid !== undefined && tid !== null;
  const hasPid = pid !== undefined && pid !== null;
  const hasUid = uid !== undefined && uid !== null;
  const hasKind = kind !== undefined;
  const isThreadTrack = threadTrack !== undefined && threadTrack;
  const isUidTrack = uidTrack !== undefined && uidTrack;

  // If we don't have any useful information (better than
  // upid/utid) we show the track kind to help with tracking
  // down where this is coming from.
  const kindSuffix = hasKind ? ` (${kind})` : '';
  const machineLabel = maybeMachineLabel(machine ?? undefined);

  if (isThreadTrack && hasName && hasTid) {
    return `${name} (${tid})`;
  } else if (isUidTrack && hasName && hasUserName) {
    return `${name} (${userName})`;
  } else if (isUidTrack && hasName && hasUid) {
    return `${name} ${uid}`;
  } else if (hasName) {
    return `${name}`;
  } else if (hasThreadName && hasTid) {
    return `${threadName} ${tid}`;
  } else if (hasTid) {
    return `Thread ${tid}`;
  } else if (hasUpid && hasPid && hasProcessName) {
    return `${processName} ${pid}${machineLabel}`;
  } else if (hasUpid && hasPid) {
    return `Process ${pid}${machineLabel}`;
  } else if (hasUpid) {
    return `upid: ${upid}${kindSuffix}`;
  } else if (hasUtid) {
    return `utid: ${utid}${kindSuffix}`;
  } else if (hasUid) {
    return `uid: ${uid}${kindSuffix}`;
  } else if (hasKind) {
    return `Unnamed ${kind}`;
  }
  return 'Unknown';
}

export function getThreadOrProcUri(
  upid: number | null,
  utid: number | null,
): string {
  if (exists(upid)) {
    return `/process_${upid}`;
  } else if (exists(utid)) {
    return `/thread_${utid}`;
  } else {
    throw new Error('No upid or utid defined...');
  }
}

export function getThreadUriPrefix(upid: number | null, utid: number): string {
  if (exists(upid)) {
    return `/process_${upid}/thread_${utid}`;
  } else {
    return `/thread_${utid}`;
  }
}

// Returns the time span of the current selection, or the visible window if
// there is no current selection.
export async function getTimeSpanOfSelectionOrVisibleWindow(
  trace: Trace,
): Promise<TimeSpan> {
  const range = await trace.selection.findTimeRangeOfSelection();
  if (exists(range)) {
    return new TimeSpan(range.start, range.end);
  } else {
    return trace.timeline.visibleWindow.toTimeSpan();
  }
}
