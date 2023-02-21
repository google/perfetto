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

import {fromNs} from '../common/time';
import {globals} from './globals';

// Type-safe aliases for various flavours of ints Trace Processor exposes
// (e.g. timestamp or ids into a given SQL table) and functions to work with
// them.
//
// These rely on TypeScript's type branding: extending a number with additional
// compile-time-only type information, which prevents "implicit" conversions
// between different ids.

// Timestamp (in nanoseconds) in the same time domain as Trace Processor is
// exposing.
export type TPTimestamp = number&{
  __type: 'TPTimestamp'
}

// TODO: unify this with common/time.ts.
export function toTraceTime(ts: TPTimestamp): number {
  return fromNs(ts) - globals.state.traceTime.startSec;
}

// Unique id for a process, id into |process| table.
export type Upid = number&{
  __type: 'Upid'
}

export function asUpid(v: number): Upid;
export function asUpid(v?: number): Upid|undefined;
export function asUpid(v?: number): Upid|undefined {
  return v as (Upid | undefined);
}

// Unique id for a thread, id into |thread| table.
export type Utid = number&{
  __type: 'Utid'
}

export function asUtid(v: number): Utid;
export function asUtid(v?: number): Utid|undefined;
export function asUtid(v?: number): Utid|undefined {
  return v as (Utid | undefined);
}

// Id into |sched| SQL table.
export type SchedSqlId = number&{
  __type: 'SchedSqlId'
}

// Id into |thread_state| SQL table.
export type ThreadStateSqlId = number&{
  __type: 'ThreadStateSqlId'
}
