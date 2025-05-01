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

import m from 'mithril';
import {duration, time, TimeSpan} from '../base/time';
import {Dataset, DatasetSchema} from '../trace_processor/dataset';
import {Engine} from '../trace_processor/engine';
import {ColumnDef, Sorting, ThreadStateExtra} from './aggregation';
import {Track} from './track';
import {arrayEquals} from '../base/array_utils';

export interface ContentWithLoadingFlag {
  readonly isLoading: boolean;
  readonly content: m.Children;
}

export interface AreaSelectionTab {
  // Unique id for this tab.
  readonly id: string;

  // A name for this tab.
  readonly name: string;

  // Defines the sort order of this tab - higher values appear first.
  readonly priority?: number;

  /**
   * Called every Mithril render cycle to render the content of the tab. The
   * returned content will be displayed inside the current selection tab.
   *
   * If undefined is returned then the tab handle will be hidden, which gives
   * the tab the option to dynamically remove itself from the list of tabs if it
   * has nothing relevant to show.
   *
   * The |isLoading| flag is used to avoid flickering. If set to true, we keep
   * hold of the the previous vnodes, rendering them instead, for up to 50ms
   * before switching to the new content. This avoids very fast load times
   * from causing flickering loading screens, which can be somewhat jarring.
   */
  render(selection: AreaSelection): ContentWithLoadingFlag | undefined;
}

/**
 * Compare two area selections for equality. Returns true if the selections are
 * equivalent, false otherwise.
 */
export function areaSelectionsEqual(a: AreaSelection, b: AreaSelection) {
  if (a.start !== b.start) return false;
  if (a.end !== b.end) return false;
  if (!arrayEquals(a.trackUris, b.trackUris)) {
    return false;
  }
  return true;
}

export interface SelectionManager {
  readonly selection: Selection;

  /**
   * Provides a list of registered area selection tabs.
   */
  readonly areaSelectionTabs: ReadonlyArray<AreaSelectionTab>;

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

  /**
   * Register a new tab under the area selection details panel.
   */
  registerAreaSelectionTab(tab: AreaSelectionTab): void;
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

  // Note: dur can be 0 for instant events or -1 for DNF slices. Will be
  // undefined if this selection has no duration, i.e. profile / counter
  // samples.
  readonly dur?: duration;

  // Optional additional information.
  // TODO(stevegolton): Find an elegant way of moving this information out of
  // the core.
  readonly utid?: number;
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

  // This array contains the resolved Tracks from Area.trackUris. The resolution
  // is done by SelectionManager whenever a kind='area' selection is performed.
  readonly tracks: ReadonlyArray<Track>;
}

export interface NoteSelection {
  readonly kind: 'note';
  readonly id: string;
}

export interface EmptySelection {
  readonly kind: 'empty';
}

export interface SqlSelectionResolver {
  readonly sqlTableName: string;
  readonly callback: (
    id: number,
    sqlTable: string,
  ) => Promise<{trackUri: string; eventId: number} | undefined>;
}
