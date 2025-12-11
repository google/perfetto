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
import {arrayEquals} from '../base/array_utils';
import {duration, time, TimeSpan} from '../base/time';
import {Track} from './track';

export interface ContentWithLoadingFlag {
  /**
   * Indicates whether the content is still loading. If true, a loading spinner
   * is shown instead of the tab content.
   */
  readonly isLoading: boolean;

  /**
   * Content to render inside the selection tab.
   */
  readonly content: m.Children;

  /**
   * Optional buttons displayed on the RHS of the aggregation panel.
   */
  readonly buttons?: m.Children;
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

  /**
   * Clears the current selection, selects nothing.
   */
  clearSelection(): void;

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
   * Resolves events via a sql table name + ids.
   *
   * @param sqlTableName - The name of the SQL table to resolve.
   * @param ids - The IDs of the events in that table.
   */
  resolveSqlEvents(
    sqlTableName: string,
    ids: ReadonlyArray<number>,
  ): Promise<ReadonlyArray<{eventId: number; trackUri: string}>>;

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

  /**
   * Scroll the timeline horizontally and vertically to reveal the currently
   * selected entity.
   *
   * @param behavior - Controls zoom behavior:
   *   - 'pan' (default): Just pan to center without changing zoom.
   *   - 'focus': Smart zoom that centers and zooms to fit the selection.
   */
  scrollToSelection(behavior?: 'pan' | 'focus'): void;

  /**
   * Returns the smallest time span that contains the currently selected entity.
   *
   * @returns The time span, if a timeline entity is selected, otherwise
   * undefined.
   */
  getTimeSpanOfSelection(): TimeSpan | undefined;

  /**
   * Register a new tab under the area selection details panel.
   */
  registerAreaSelectionTab(tab: AreaSelectionTab): void;
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
}

export interface Area {
  readonly start: time;
  readonly end: time;
  readonly trackUris: ReadonlyArray<string>;
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
