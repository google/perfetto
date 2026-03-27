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

export interface HeapInfo {
  name: string;
  java: number;
  native_: number;
}

export interface DuplicateBitmapGroup {
  /** Content hash or dimension-based fallback key used for grouping. */
  groupKey: string;
  width: number;
  height: number;
  count: number;
  totalBytes: number;
  wastedBytes: number;
}

export interface DuplicateStringGroup {
  value: string;
  count: number;
  totalBytes: number;
  wastedBytes: number;
}

export interface DuplicateArrayGroup {
  className: string;
  arrayHash: string;
  count: number;
  totalBytes: number;
  wastedBytes: number;
}

export interface OverviewData {
  instanceCount: number;
  heaps: HeapInfo[];
  duplicateBitmaps?: DuplicateBitmapGroup[];
  duplicateStrings?: DuplicateStringGroup[];
  duplicateArrays?: DuplicateArrayGroup[];
  /** True when HPROF field values are available (heap_graph_primitive). */
  hasFieldValues: boolean;
}

export type PrimOrRef =
  | {kind: 'prim'; v: string}
  | {
      kind: 'ref';
      id: number;
      display: string;
      str: string | null;
      shallowJava?: number;
      shallowNative?: number;
      retainedJava?: number;
      retainedNative?: number;
      reachableJava?: number;
      reachableNative?: number;
      reachableCount?: number;
    };

export interface PathEntry {
  row: InstanceRow;
  field: string;
  isDominator: boolean;
}

export interface InstanceRow {
  id: number;
  display: string;
  className: string;
  isRoot: boolean;
  rootTypeNames: string[] | null;
  reachabilityName: string;
  heap: string;
  shallowJava: number;
  shallowNative: number;
  retainedTotal: number;
  retainedCount: number;
  reachableSize: number | null;
  reachableNative: number | null;
  reachableCount: number | null;
  retainedByHeap: {heap: string; java: number; native_: number}[];
  str: string | null;
  referent: InstanceRow | null;
  isPlaceHolder?: boolean;
}

export interface InstanceDetail {
  row: InstanceRow;
  isClassObj: boolean;
  isArrayInstance: boolean;
  isClassInstance: boolean;
  classObjRow: InstanceRow | null;
  forClassName: string | null;
  superClassObjId: number | null;
  instanceSize: number;
  staticFields: {name: string; typeName: string; value: PrimOrRef}[];
  instanceFields: {name: string; typeName: string; value: PrimOrRef}[];
  elemTypeName: string | null;
  arrayLength: number;
  arrayElems: {idx: number; value: PrimOrRef}[];
  bitmap: {
    width: number;
    height: number;
    format: string;
    data: Uint8Array;
  } | null;
  reverseRefs: InstanceRow[];
  dominated: InstanceRow[];
  pathFromRoot: PathEntry[] | null;
  isUnreachablePath?: boolean;
}

export interface ClassRow {
  className: string;
  count: number;
  shallowSize: number;
  nativeSize: number;
  retainedSize: number;
  retainedNativeSize: number;
  retainedCount: number;
  reachableSize: number | null;
  reachableNativeSize: number | null;
  reachableCount: number | null;
  heap: string;
}

export interface BitmapListRow {
  row: InstanceRow;
  width: number;
  height: number;
  pixelCount: number;
  hasPixelData: boolean;
  density: number;
  /** Content hash of the compressed pixel buffer, null when unavailable. */
  bufferHash: string | null;
}

export interface StringListRow {
  id: number;
  value: string;
  length: number;
  retainedSize: number;
  reachableSize: number | null;
  reachableNativeSize: number | null;
  reachableCount: number | null;
  shallowSize: number;
  nativeSize: number;
  heap: string;
  className: string;
  display: string;
}
