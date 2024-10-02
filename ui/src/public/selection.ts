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

import {time, duration, TimeSpan} from '../base/time';
import {Optional} from '../base/utils';
import {Engine} from '../trace_processor/engine';
import {ColumnDef, Sorting, ThreadStateExtra} from './aggregation';
import {GenericSliceDetailsTabConfigBase} from './details_panel';
import {TrackDescriptor} from './track';

export interface SelectionManager {
  readonly selection: Selection;
  readonly legacySelection: LegacySelection | null;

  findTimeRangeOfSelection(): Optional<TimeSpan>;
  clear(): void;

  /**
   * Select a track event.
   *
   * @param trackUri - The URI of the track to select.
   * @param eventId - The value of the events ID column.
   * @param opts - Additional options.
   */
  selectTrackEvent(
    trackUri: string,
    eventId: number,
    opts?: SelectionOpts,
  ): void;

  /**
   * Select a track event via a sql table name + id.
   *
   * @param sqlTableName - The name of the SQL table to resolve.
   * @param id - The ID of the event in that table.
   * @param opts - Additional options.
   */
  selectSqlEvent(sqlTableName: string, id: number, opts?: SelectionOpts): void;

  /**
   * Select a legacy selection.
   *
   * @param selection - The legacy selection to select.
   * @param opts - Additional options.
   */
  selectLegacy(selection: LegacySelection, opts?: SelectionOpts): void;

  /**
   * Create an area selection for the purposes of aggregation.
   *
   * @param args - The area to select.
   * @param opts - Additional options.
   */
  selectArea(args: Area, opts?: SelectionOpts): void;

  scrollToCurrentSelection(): void;
  registerAreaSelectionAggreagtor(aggr: AreaSelectionAggregator): void;

  // TODO(primiano): I don't undertsand what this generic slice is, but now
  // is exposed to plugins. For now i'm just carrying it forward.
  selectGenericSlice(args: {
    id: number;
    sqlTableName: string;
    start: time;
    duration: duration;
    trackUri: string;
    detailsPanelConfig: {
      kind: string;
      config: GenericSliceDetailsTabConfigBase;
    };
  }): void;

  /**
   * Register a new SQL selection resolver.
   *
   * A resolver consists of a SQL table name and a callback. When someone
   * expresses an interest in selecting a slice on a matching table, the
   * callback is called which can return a selection object or undefined.
   */
  registerSqlSelectionResolver(resolver: SqlSelectionResolver): void;
}

export interface AreaSelectionAggregator {
  readonly id: string;
  createAggregateView(engine: Engine, area: AreaSelection): Promise<boolean>;
  getExtra(
    engine: Engine,
    area: AreaSelection,
  ): Promise<ThreadStateExtra | void>;
  getTabName(): string;
  getDefaultSorting(): Sorting;
  getColumnDefinitions(): ColumnDef[];
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
  scrollToSelection?: boolean; // Default: false.
}

// LEGACY Selection types:

export interface LegacySelectionWrapper {
  readonly kind: 'legacy';
  readonly legacySelection: LegacySelection;
}

export type LegacySelection = (
  | HeapProfileSelection
  | CpuProfileSampleSelection
  | PerfSamplesSelection
  | LogSelection
  | GenericSliceSelection
) & {trackUri?: string};

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

export interface SingleSelection extends TrackEventDetails {
  readonly kind: 'single';
  readonly trackUri: string;
  readonly eventId: number;
}

export interface TrackEventDetails {
  // ts and dur are required by the core, and must be provided.
  readonly ts: time;
  // Note: dur can be -1 for instant events.
  readonly dur: duration;

  // Optional additional information.
  // TODO(stevegolton): Find an elegant way of moving this information out of
  // the core.
  readonly wakeupTs?: time;
  readonly wakerCpu?: number;
  readonly utid?: number;
  readonly tableName?: string;
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

  // This array contains the resolved TrackDescriptor from Area.trackUris.
  // The resolution is done by SelectionManager whenever a kind='area' selection
  // is performed.
  readonly tracks: ReadonlyArray<TrackDescriptor>;
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

export interface SqlSelectionResolver {
  readonly sqlTableName: string;
  readonly callback: (
    id: number,
    sqlTable: string,
  ) => Promise<{trackUri: string; eventId: number} | undefined>;
}
