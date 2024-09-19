// Copyright (C) 2024 The Android Open Source Project
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

import {time, duration} from '../base/time';
import {GenericSliceDetailsTabConfigBase} from './details_panel';

export interface SelectionManager {
  readonly selection: Selection;
  clear(): void;
  setLegacy(args: LegacySelection, opts?: SelectionOpts): void;
  scrollToCurrentSelection(): void;
}

export type Selection =
  | SingleSelection
  | AreaSelection
  | NoteSelection
  | UnionSelection
  | EmptySelection
  | LegacySelectionWrapper;

/** Defines how changes to selection affect the rest of the UI state */
export interface SelectionOpts {
  clearSearch?: boolean; // Default: true.
  switchToCurrentSelectionTab?: boolean; // Default: true.
  pendingScrollId?: number; // Default: no auto-scroll.
}

// LEGACY Selection types:

export interface LegacySelectionWrapper {
  readonly kind: 'legacy';
  readonly legacySelection: LegacySelection;
}

export type LegacySelection = (
  | SliceSelection
  | HeapProfileSelection
  | CpuProfileSampleSelection
  | ThreadSliceSelection
  | ThreadStateSelection
  | PerfSamplesSelection
  | LogSelection
  | GenericSliceSelection
) & {trackUri?: string};

export type SelectionKind = LegacySelection['kind']; // 'THREAD_STATE' | 'SLICE' ...

export interface SliceSelection {
  readonly kind: 'SCHED_SLICE';
  readonly id: number;
}

export interface HeapProfileSelection {
  readonly kind: 'HEAP_PROFILE';
  readonly id: number;
  readonly upid: number;
  readonly ts: time;
  readonly type: ProfileType;
}

export interface PerfSamplesSelection {
  readonly kind: 'PERF_SAMPLES';
  readonly id: number;
  readonly utid?: number;
  readonly upid?: number;
  readonly leftTs: time;
  readonly rightTs: time;
  readonly type: ProfileType;
}

export interface CpuProfileSampleSelection {
  readonly kind: 'CPU_PROFILE_SAMPLE';
  readonly id: number;
  readonly utid: number;
  readonly ts: time;
}

export interface ThreadSliceSelection {
  readonly kind: 'SLICE';
  readonly id: number;
  readonly table?: string;
}

export interface ThreadStateSelection {
  readonly kind: 'THREAD_STATE';
  readonly id: number;
}

export interface LogSelection {
  readonly kind: 'LOG';
  readonly id: number;
  readonly trackUri: string;
}

export interface GenericSliceSelection {
  readonly kind: 'GENERIC_SLICE';
  readonly id: number;
  readonly sqlTableName: string;
  readonly start: time;
  readonly duration: duration;
  // NOTE: this config can be expanded for multiple details panel types.
  readonly detailsPanelConfig: {
    readonly kind: string;
    readonly config: GenericSliceDetailsTabConfigBase;
  };
}

// New Selection types:

export interface SingleSelection {
  readonly kind: 'single';
  readonly trackUri: string;
  readonly eventId: number;
}

export interface Area {
  readonly start: time;
  readonly end: time;
  // TODO(primiano): this should be ReadonlyArray<> after the pivot table state
  // doesn't use State/Immer anymore.
  readonly trackUris: string[];
}

export interface AreaSelection extends Area {
  readonly kind: 'area';
}

export interface NoteSelection {
  readonly kind: 'note';
  readonly id: string;
}

export interface UnionSelection {
  readonly kind: 'union';
  readonly selections: ReadonlyArray<Selection>;
}

export interface EmptySelection {
  readonly kind: 'empty';
}

export enum ProfileType {
  HEAP_PROFILE = 'heap_profile',
  MIXED_HEAP_PROFILE = 'heap_profile:com.android.art,libc.malloc',
  NATIVE_HEAP_PROFILE = 'heap_profile:libc.malloc',
  JAVA_HEAP_SAMPLES = 'heap_profile:com.android.art',
  JAVA_HEAP_GRAPH = 'graph',
  PERF_SAMPLE = 'perf',
}

export function profileType(s: string): ProfileType {
  if (s === 'heap_profile:libc.malloc,com.android.art') {
    s = 'heap_profile:com.android.art,libc.malloc';
  }
  if (Object.values(ProfileType).includes(s as ProfileType)) {
    return s as ProfileType;
  }
  if (s.startsWith('heap_profile')) {
    return ProfileType.HEAP_PROFILE;
  }
  throw new Error('Unknown type ${s}');
}
