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

import type {time} from '../../base/time';
import type {OomeDetails} from '../dev.perfetto.HeapProfile/oome_callstack_common';

export type {OomeDetails};

export interface HeapDump {
  readonly upid: number;
  readonly ts: time;
  readonly processName: string | null;
  readonly pid: number;
}

export interface HeapInfo {
  readonly name: string;
  readonly java: number;
  readonly native_: number;
}

export interface DuplicateBitmapGroup {
  /** Content hash or dimension-based fallback key used for grouping. */
  readonly groupKey: string;
  readonly width: number;
  readonly height: number;
  readonly count: number;
  readonly totalBytes: number;
  readonly wastedBytes: number;
}

export interface DuplicateStringGroup {
  readonly value: string;
  readonly count: number;
  readonly totalBytes: number;
  readonly wastedBytes: number;
}

export interface DuplicateArrayGroup {
  readonly className: string;
  readonly arrayHash: string;
  readonly count: number;
  readonly totalBytes: number;
  readonly wastedBytes: number;
}

export interface OomeData {
  readonly upid: number;
  readonly ts: time;
  readonly details: OomeDetails;
}

export interface OverviewData {
  readonly reachableInstanceCount: number;
  readonly unreachableInstanceCount: number;
  readonly classCount: number;
  readonly heaps: readonly HeapInfo[];
  readonly duplicateBitmaps?: readonly DuplicateBitmapGroup[];
  readonly duplicateStrings?: readonly DuplicateStringGroup[];
  readonly duplicateArrays?: readonly DuplicateArrayGroup[];
  /** Process oom_score_adj at the dump instant; null if the trace has none. */
  readonly oomScore: number | null;
  /** oom_adj bucket name from the stdlib (e.g. "cached"); null if unavailable. */
  readonly oomBucket: string | null;
  /** The anon RSS + swap size of the process (in bytes) at the time of the heap dump. */
  readonly anonRssAndSwapSize: bigint | null;
  /** The dmabuf size of the process (in bytes) at the time of the heap dump. */
  readonly dmabufRssSize: bigint | null;
  /** The process uptime at the time of the heap dump. */
  readonly processUptime: bigint | null;
  /** OOME details, if the dump was triggered by an OutOfMemoryError. */
  readonly oome: OomeDetails | undefined;
}

export type PrimOrRef =
  | {readonly kind: 'prim'; readonly v: string}
  | {
      readonly kind: 'ref';
      readonly id: number;
      readonly display: string;
      readonly str: string | null;
      readonly shallowJava?: number;
      readonly shallowNative?: number;
      readonly retainedJava?: number;
      readonly retainedNative?: number;
      reachableJava?: number;
      reachableNative?: number;
      reachableCount?: number;
    };

export interface PathEntry {
  readonly row: InstanceRow;
  readonly field: string;
  readonly isDominator: boolean;
}

export interface InstanceRow {
  readonly id: number;
  readonly display: string;
  readonly className: string;
  readonly isRoot: boolean;
  readonly rootTypeNames: readonly string[] | null;
  readonly reachabilityName: string;
  readonly heap: string;
  readonly shallowJava: number;
  readonly shallowNative: number;
  readonly retainedTotal: number;
  readonly retainedCount: number;
  reachableSize: number | null;
  reachableNative: number | null;
  reachableCount: number | null;
  readonly retainedByHeap: readonly {
    readonly heap: string;
    readonly java: number;
    readonly native_: number;
  }[];
  readonly str: string | null;
  readonly referent: InstanceRow | null;
  readonly isPlaceHolder?: boolean;
}

export interface Field {
  readonly name: string;
  readonly typeName: string;
  readonly value: PrimOrRef;
}

export interface ArrayElem {
  readonly idx: number;
  readonly value: PrimOrRef;
}

export interface InstanceDetail {
  readonly row: InstanceRow;
  readonly isClassObj: boolean;
  readonly isArrayInstance: boolean;
  readonly isClassInstance: boolean;
  readonly classObjRow: InstanceRow | null;
  readonly instanceSize: number;
  /** Superclass chain ordered starting-class first. */
  readonly classHierarchy: readonly string[];
  readonly staticFields: readonly Field[];
  readonly instanceFields: readonly Field[];
  readonly elemTypeName: string | null;
  readonly arrayLength: number;
  readonly arrayElems: readonly ArrayElem[];
  readonly bitmap: {
    readonly width: number;
    readonly height: number;
    readonly format: string;
    readonly data: Uint8Array<ArrayBuffer>;
  } | null;
  readonly reverseRefs: readonly InstanceRow[];
  readonly dominated: readonly InstanceRow[];
  readonly dominatorPath: readonly PathEntry[] | null;
  readonly shortestPath: readonly PathEntry[] | null;
}

export interface BitmapListRow {
  readonly row: InstanceRow;
  readonly width: number;
  readonly height: number;
  readonly pixelCount: number;
  readonly hasPixelData: boolean;
  readonly density: number;
  /** Content hash of the compressed pixel buffer, null when unavailable. */
  readonly bufferHash: string | null;
  /**
   * Pixel-storage backing decoded from `Bitmap.mId` via the
   * `android.memory.heap_graph.bitmap` stdlib module. One of
   * 'heap' | 'ashmem' | 'hardware' | 'wrapped_pixel_ref'.
   * 'heap' = malloc'd in this process (real RAM cost per copy);
   * 'ashmem' = shared kernel memory (PSS-shared across processes);
   * 'hardware' = AHardwareBuffer (GPU memory).
   */
  readonly storageType: string | null;
  /** Encoded `Bitmap.mId`. */
  readonly bitmapId: bigint | null;
  /**
   * Encoded `Bitmap.mSourceId` for parcel-received Bitmaps; null when the
   * Bitmap was locally allocated (raw -1 sentinel canonicalised).
   */
  readonly sourceId: bigint | null;
  /** Sender pid decoded from sourceId. Null when sourceId is null. */
  readonly sourcePid: number | null;
  /** Sender's pixel storage type at writeToParcel time. */
  readonly sourceStorageType: string | null;
  /**
   * Sender's process name resolved against `process` at the heap dump's
   * timestamp. Requires the trace to include process info (e.g. captured
   * via `linux.process_stats` alongside the HPROF dump). Null otherwise.
   */
  readonly sourceProcessName: string | null;
}

export interface StringListRow {
  readonly id: number;
  readonly value: string;
  readonly length: number;
  readonly retainedSize: number;
  readonly reachableSize: number | null;
  readonly reachableNativeSize: number | null;
  readonly reachableCount: number | null;
  readonly shallowSize: number;
  readonly nativeSize: number;
  readonly heap: string;
  readonly className: string;
  readonly display: string;
}
