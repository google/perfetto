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
import {Dataset, DatasetSchema} from '../trace_processor/dataset';
import {Engine} from '../trace_processor/engine';
import {ColumnDef, Sorting, ThreadStateExtra} from './aggregation';
import {TrackDescriptor} from './track';

export interface SelectionManager {
  readonly selection: Selection;

  findTimeRangeOfSelection(): TimeSpan | undefined;
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
   * Select a track.
   *
   * @param trackUri - The URI for the track to select.
   * @param opts - Additional options.
   */
  selectTrack(trackUri: string, opts?: SelectionOpts): void;

  /**
   * Select a track event via a sql table name + id.
   *
   * @param sqlTableName - The name of the SQL table to resolve.
   * @param id - The ID of the event in that table.
   * @param opts - Additional options.
   */
  selectSqlEvent(sqlTableName: string, id: number, opts?: SelectionOpts): void;

  /**
   * Create an area selection for the purposes of aggregation.
   *
   * @param args - The area to select.
   * @param opts - Additional options.
   */
  selectArea(args: Area, opts?: SelectionOpts): void;

  scrollToCurrentSelection(): void;
  registerAreaSelectionAggregator(aggr: AreaSelectionAggregator): void;

  /**
   * Register a new SQL selection resolver.
   *
   * A resolver consists of a SQL table name and a callback. When someone
   * expresses an interest in selecting a slice on a matching table, the
   * callback is called which can return a selection object or undefined.
   */
  registerSqlSelectionResolver(resolver: SqlSelectionResolver): void;
}

/**
 * Aggregator tabs are displayed in descending order of specificity, determined
 * by the following precedence hierarchy:
 * 1. Aggregators explicitly defining a `trackKind` string take priority over
 *    those that do not.
 * 2. Otherwise, aggregators with schemas containing a greater number of keys
 *    (higher specificity) are prioritized over those with fewer keys.
 * 3. In cases of identical specificity, tabs are ranked based on their
 *    registration order.
 */
export interface AreaSelectionAggregator {
  readonly id: string;

  /**
   * If defined, the dataset passed to `createAggregateView` will only contain
   * tracks with a matching `kind` tag.
   */
  readonly trackKind?: string;

  /**
   * If defined, the dataset passed to `createAggregateView` will only contain
   * tracks that export datasets that implement this schema.
   */
  readonly schema?: DatasetSchema;

  /**
   * Creates a view for the aggregated data corresponding to the selected area.
   *
   * The dataset provided will be filtered based on the `trackKind` and `schema`
   * if these properties are defined.
   *
   * @param engine - The query engine used to execute queries.
   * @param area - The currently selected area to aggregate.
   * @param dataset - The dataset representing a union of the data in the
   * selected tracks.
   */
  createAggregateView(
    engine: Engine,
    area: AreaSelection,
    dataset?: Dataset,
  ): Promise<boolean>;
  getExtra(
    engine: Engine,
    area: AreaSelection,
    dataset?: Dataset,
  ): Promise<ThreadStateExtra | void>;
  getTabName(): string;
  getDefaultSorting(): Sorting;
  getColumnDefinitions(): ColumnDef[];
}

export type Selection =
  | TrackEventSelection
  | TrackSelection
  | AreaSelection
  | NoteSelection
  | EmptySelection;

/** Defines how changes to selection affect the rest of the UI state */
export interface SelectionOpts {
  clearSearch?: boolean; // Default: true.
  switchToCurrentSelectionTab?: boolean; // Default: true.
  scrollToSelection?: boolean; // Default: false.
}

export interface TrackEventSelection extends TrackEventDetails {
  readonly kind: 'track_event';
  readonly trackUri: string;
  readonly eventId: number;
}

export interface TrackSelection {
  readonly kind: 'track';
  readonly trackUri: string;
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
  readonly upid?: number;
  readonly utid?: number;
  readonly profileType?: ProfileType;
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
  INSTRUMENTS_SAMPLE = 'instruments',
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
