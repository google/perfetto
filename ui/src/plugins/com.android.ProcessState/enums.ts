// Copyright (C) 2026 The Android Open Source Project
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

import type m from 'mithril';

// Display names for the AMS enums the data source records as raw ints. The raw
// value always stays in the table (queryable / sortable); these maps only drive
// what the grids SHOW. Values mirror the platform exactly (ProcessStateEnum,
// ProcessList, ActivityManagerInternal, ActivityManager, ServiceInfo,
// app_enums.proto) — keep them in sync if the platform changes.

// android.app.ProcessStateEnum
const PROCESS_STATE: {readonly [k: number]: string} = {
  [-1]: 'UNKNOWN',
  0: 'PERSISTENT',
  1: 'PERSISTENT_UI',
  2: 'TOP',
  3: 'BOUND_TOP',
  4: 'FOREGROUND_SERVICE',
  5: 'BOUND_FOREGROUND_SERVICE',
  6: 'IMPORTANT_FOREGROUND',
  7: 'IMPORTANT_BACKGROUND',
  8: 'TRANSIENT_BACKGROUND',
  9: 'BACKUP',
  10: 'SERVICE',
  11: 'RECEIVER',
  12: 'TOP_SLEEPING',
  13: 'HEAVY_WEIGHT',
  14: 'HOME',
  15: 'LAST_ACTIVITY',
  16: 'CACHED_ACTIVITY',
  17: 'CACHED_ACTIVITY_CLIENT',
  18: 'CACHED_RECENT',
  19: 'CACHED_EMPTY',
  20: 'NONEXISTENT',
};

// ProcessList.SCHED_GROUP_*
const SCHED_GROUP: {readonly [k: number]: string} = {
  0: 'BACKGROUND',
  1: 'RESTRICTED',
  2: 'DEFAULT',
  3: 'TOP_APP',
  4: 'TOP_APP_BOUND',
  5: 'FOREGROUND_WINDOW',
};

// ActivityManagerInternal.OOM_ADJ_REASON_*
const OOM_ADJ_REASON: {readonly [k: number]: string} = {
  0: 'NONE',
  1: 'ACTIVITY',
  2: 'FINISH_RECEIVER',
  3: 'START_RECEIVER',
  4: 'BIND_SERVICE',
  5: 'UNBIND_SERVICE',
  6: 'START_SERVICE',
  7: 'GET_PROVIDER',
  8: 'REMOVE_PROVIDER',
  9: 'UI_VISIBILITY',
  10: 'ALLOWLIST',
  11: 'PROCESS_BEGIN',
  12: 'PROCESS_END',
  13: 'SHORT_FGS_TIMEOUT',
  14: 'SYSTEM_INIT',
  15: 'BACKUP',
  16: 'SHELL',
  17: 'REMOVE_TASK',
  18: 'UID_IDLE',
  19: 'STOP_SERVICE',
  20: 'EXECUTING_SERVICE',
  21: 'RESTRICTION_CHANGE',
  22: 'COMPONENT_DISABLED',
  23: 'FOLLOW_UP',
  24: 'RECONFIGURATION',
  25: 'SERVICE_BINDER_CALL',
  26: 'BATCH_UPDATE_REQUEST',
};

// ActivityManager.RESTRICTION_LEVEL_*
const RESTRICTION_LEVEL: {readonly [k: number]: string} = {
  0: 'UNKNOWN',
  10: 'UNRESTRICTED',
  20: 'EXEMPTED',
  30: 'ADAPTIVE_BUCKET',
  40: 'RESTRICTED_BUCKET',
  50: 'BACKGROUND_RESTRICTED',
  60: 'FORCE_STOPPED',
  70: 'USER_LAUNCH_ONLY',
  90: 'CUSTOM',
  100: 'MAX',
};

// UsageStatsManager.STANDBY_BUCKET_*
const STANDBY_BUCKET: {readonly [k: number]: string} = {
  5: 'EXEMPTED',
  10: 'ACTIVE',
  20: 'WORKING_SET',
  30: 'FREQUENT',
  40: 'RARE',
  45: 'RESTRICTED',
  50: 'NEVER',
};

// Bitmask flag → name. [bitValue, name] pairs.
// ActivityManager.PROCESS_CAPABILITY_*
const CAPABILITY: ReadonlyArray<[number, string]> = [
  [1 << 0, 'FG_LOCATION'],
  [1 << 1, 'FG_CAMERA'],
  [1 << 2, 'FG_MICROPHONE'],
  [1 << 3, 'POWER_RESTRICTED_NETWORK'],
  [1 << 4, 'BFSL'],
  [1 << 5, 'USER_RESTRICTED_NETWORK'],
  [1 << 6, 'FG_AUDIO_CONTROL'],
  [1 << 7, 'CPU_TIME'],
  [1 << 8, 'IMPLICIT_CPU_TIME'],
];

// ServiceInfo.FOREGROUND_SERVICE_TYPE_*
const FGS_TYPE: ReadonlyArray<[number, string]> = [
  [1 << 0, 'DATA_SYNC'],
  [1 << 1, 'MEDIA_PLAYBACK'],
  [1 << 2, 'PHONE_CALL'],
  [1 << 3, 'LOCATION'],
  [1 << 4, 'CONNECTED_DEVICE'],
  [1 << 5, 'MEDIA_PROJECTION'],
  [1 << 6, 'CAMERA'],
  [1 << 7, 'MICROPHONE'],
  [1 << 8, 'HEALTH'],
  [1 << 9, 'REMOTE_MESSAGING'],
  [1 << 10, 'SYSTEM_EXEMPTED'],
  [1 << 11, 'SHORT_SERVICE'],
  [1 << 12, 'FILE_MANAGEMENT'],
  [1 << 13, 'MEDIA_PROCESSING'],
  [1 << 30, 'SPECIAL_USE'],
];

// AppProtoEnums.HOSTING_COMPONENT_TYPE_*
const HOSTING_TYPE: ReadonlyArray<[number, string]> = [
  [0x0001, 'SYSTEM'],
  [0x0002, 'PERSISTENT'],
  [0x0004, 'BACKUP'],
  [0x0008, 'INSTRUMENTATION'],
  [0x0010, 'ACTIVITY'],
  [0x0020, 'BROADCAST_RECEIVER'],
  [0x0040, 'PROVIDER'],
  [0x0080, 'STARTED_SERVICE'],
  [0x0100, 'FOREGROUND_SERVICE'],
  [0x0200, 'BOUND_SERVICE'],
];

// "NAME (id)" for a scalar enum, or just the id if unknown. null/undefined → ''.
function scalar(map: {readonly [k: number]: string}, v: unknown): string {
  if (v === null || v === undefined || v === '') return '';
  const n = Number(v);
  const name = map[n];
  return name === undefined ? String(n) : `${name} (${n})`;
}

// "FLAG_A | FLAG_B (0xNN)" for a bitmask, "none (0)" when zero.
function bits(masks: ReadonlyArray<[number, string]>, v: unknown): string {
  if (v === null || v === undefined || v === '') return '';
  const n = Number(v);
  if (n === 0) return 'none (0)';
  const on = masks.filter(([bit]) => (n & bit) !== 0).map(([, name]) => name);
  const known = masks.reduce((acc, [bit]) => acc | bit, 0);
  const extra = n & ~known; // bits we don't have a name for
  if (extra !== 0) on.push(`0x${extra.toString(16)}`);
  return `${on.join(' | ')} (0x${n.toString(16)})`;
}

// Column name → cell renderer, applied by gridSchema so every grid that shows
// one of these columns renders the human-readable enum/flags automatically.
// Scalar proc-state columns (every *_proc_state and the uid/global variants).
const PROC_STATE_COLS = [
  'cur_proc_state',
  'set_proc_state',
  'rep_proc_state',
  'cur_raw_proc_state',
  'cached_proc_state',
  'adj_source_proc_state',
  'effective_proc_state',
  'top_process_state',
];
const SCHED_GROUP_COLS = [
  'cur_sched_group',
  'set_sched_group',
  'cached_sched_group',
  'broadcast_receiver_sched_group',
  'final_sched_group',
];
const CAPABILITY_COLS = [
  'cur_capability',
  'set_capability',
  'effective_capability',
];
const FGS_TYPE_COLS = ['fg_service_types', 'foreground_service_type'];
const HOSTING_COLS = [
  'hosting_component_types',
  'hosting_component_types_for_oom_adj',
];

export const ENUM_RENDERERS: {
  readonly [col: string]: (v: unknown) => m.Children;
} = (() => {
  const r: {[col: string]: (v: unknown) => m.Children} = {};
  for (const c of PROC_STATE_COLS) r[c] = (v) => scalar(PROCESS_STATE, v);
  for (const c of SCHED_GROUP_COLS) r[c] = (v) => scalar(SCHED_GROUP, v);
  for (const c of CAPABILITY_COLS) r[c] = (v) => bits(CAPABILITY, v);
  for (const c of FGS_TYPE_COLS) r[c] = (v) => bits(FGS_TYPE, v);
  for (const c of HOSTING_COLS) r[c] = (v) => bits(HOSTING_TYPE, v);
  r['oom_adj_reason'] = (v) => scalar(OOM_ADJ_REASON, v);
  r['restriction_level'] = (v) => scalar(RESTRICTION_LEVEL, v);
  r['standby_bucket'] = (v) => scalar(STANDBY_BUCKET, v);
  return r;
})();

// Plain-string variants for the curated detail cards (no Mithril vnode needed).
export const procStateName = (v: unknown) => scalar(PROCESS_STATE, v);
export const schedGroupName = (v: unknown) => scalar(SCHED_GROUP, v);
export const capabilityNames = (v: unknown) => bits(CAPABILITY, v);
export const fgsTypeNames = (v: unknown) => bits(FGS_TYPE, v);
export const hostingTypeNames = (v: unknown) => bits(HOSTING_TYPE, v);
export const oomAdjReasonName = (v: unknown) => scalar(OOM_ADJ_REASON, v);
