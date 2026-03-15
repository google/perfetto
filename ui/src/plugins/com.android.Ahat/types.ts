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

// Display types shared between the SQL query layer and the Mithril views.

export interface HeapInfo {
  name: string;
  java: number;
  native_: number;
}

export interface DuplicateBitmapGroup {
  width: number;
  height: number;
  count: number;
  totalBytes: number;
  wastedBytes: number;
}

export interface OverviewData {
  instanceCount: number;
  heaps: HeapInfo[];
  duplicateBitmaps?: DuplicateBitmapGroup[];
}

export type PrimOrRef =
  | {kind: 'prim'; v: string}
  | {kind: 'ref'; id: number; display: string; str: string | null};

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
  pathFromRoot:
    | {
        row: InstanceRow;
        field: string;
        isDominator: boolean;
      }[]
    | null;
  isUnreachablePath?: boolean;
}

export interface ClassRow {
  className: string;
  count: number;
  shallowSize: number;
  retainedSize: number;
  heap: string;
}

export interface BitmapListRow {
  row: InstanceRow;
  width: number;
  height: number;
  pixelCount: number;
  bufferHash: string;
  hasPixelData: boolean;
  density: number;
}

export interface StringListRow {
  id: number;
  value: string;
  length: number;
  retainedSize: number;
  shallowSize: number;
  heap: string;
  className: string;
  display: string;
}
