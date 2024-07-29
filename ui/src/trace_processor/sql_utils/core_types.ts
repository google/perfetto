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

import {Brand} from '../../base/brand';

// Type-safe aliases for various flavours of ints Trace Processor exposes
// (e.g. timestamp or ids into a given SQL table) and functions to work with
// them.
//
// These rely on TypeScript's type branding: extending a number with additional
// compile-time-only type information, which prevents "implicit" conversions
// between different ids.

// Unique id for a process, id into |process| table.
export type Upid = number & {
  __type: 'Upid';
};

export function asUpid(v: number): Upid;
export function asUpid(v?: number): Upid | undefined;
export function asUpid(v?: number): Upid | undefined {
  return v as Upid | undefined;
}

// Unique id for a thread, id into |thread| table.
export type Utid = number & {
  __type: 'Utid';
};

export function asUtid(v: number): Utid;
export function asUtid(v?: number): Utid | undefined;
export function asUtid(v?: number): Utid | undefined {
  return v as Utid | undefined;
}

// Id into |slice| SQL table.
export type SliceSqlId = number & {
  __type: 'SliceSqlId';
};

export function asSliceSqlId(v: number): SliceSqlId;
export function asSliceSqlId(v?: number): SliceSqlId | undefined;
export function asSliceSqlId(v?: number): SliceSqlId | undefined {
  return v as SliceSqlId | undefined;
}

// Id into |sched| SQL table.
export type SchedSqlId = number & {
  __type: 'SchedSqlId';
};

export function asSchedSqlId(v: number): SchedSqlId;
export function asSchedSqlId(v?: number): SchedSqlId | undefined;
export function asSchedSqlId(v?: number): SchedSqlId | undefined {
  return v as SchedSqlId | undefined;
}

// Id into |thread_state| SQL table.
export type ThreadStateSqlId = number & {
  __type: 'ThreadStateSqlId';
};

export function asThreadStateSqlId(v: number): ThreadStateSqlId;
export function asThreadStateSqlId(v?: number): ThreadStateSqlId | undefined;
export function asThreadStateSqlId(v?: number): ThreadStateSqlId | undefined {
  return v as ThreadStateSqlId | undefined;
}

export type ArgSetId = Brand<number, 'ArgSetId'>;

export function asArgSetId(v: number): ArgSetId;
export function asArgSetId(v?: number): ArgSetId | undefined;
export function asArgSetId(v?: number): ArgSetId | undefined {
  return v as ArgSetId | undefined;
}

// Id into |args| SQL table.
export type ArgsId = Brand<number, 'ArgsId'>;

export function asArgId(v: number): ArgsId;
export function asArgId(v?: number): ArgsId | undefined;
export function asArgId(v?: number): ArgsId | undefined {
  return v as ArgsId | undefined;
}
