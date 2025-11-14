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

/**
 * Represents content that may be in a loading state.
 */
export interface ContentWithLoadingFlag {
  /**
   * Indicates whether the content is currently loading.
   */
  readonly isLoading: boolean;
  /**
   * The actual content to be displayed.
   */
  readonly content: m.Children;
}

/**
 * Defines a tab within the area selection details panel.
 */
export interface AreaSelectionTab {
  /**
   * Unique ID for this tab.
   */
  readonly id: string;

  /**
   * A human-readable name for this tab.
   */
  readonly name: string;

  /**
   * Defines the sort order of this tab - higher values appear first.
   */
  readonly priority?: number;

  /**
   * Called every Mithril render cycle to render the content of the tab. The
   * returned content will be displayed inside the current selection tab.
   *
   * If `undefined` is returned then the tab handle will be hidden, which gives
   * the tab the option to dynamically remove itself from the list of tabs if it
   * has nothing relevant to show.
   *
   * The `isLoading` flag is used to avoid flickering. If set to `true`, we keep
   * hold of the previous vnodes, rendering them instead, for up to 50ms
   * before switching to the new content. This avoids very fast load times
   * from causing flickering loading screens, which can be somewhat jarring.
   * @param selection The current area selection.
   * @returns The content to render, or `undefined` if the tab should be hidden.
   */
  render(selection: AreaSelection): ContentWithLoadingFlag | undefined;
}

/**
 * Represents the different types of selections that can be made in the UI.
 */
export type Selection =
  | TrackEventSelection
  | TrackSelection
  | AreaSelection
  | NoteSelection
  | EmptySelection;

/**
 * Defines how changes to selection affect the rest of the UI state.
 */
export interface SelectionOpts {
  /**
   * If `true`, clears the search input. Defaults to `true`.
   */
  readonly clearSearch?: boolean;
  /**
   * If `true`, switches to the tab relevant to the current selection. Defaults
   * to `true`.
   */
  readonly switchToCurrentSelectionTab?: boolean;
  /**
   * If `true`, scrolls the timeline to reveal the selection. Defaults to `false`.
   */
  readonly scrollToSelection?: boolean;
}

/**
 * Represents a selection of a specific track event.
 */
export interface TrackEventSelection extends TrackEventDetails {
  /**
   * The kind of selection, always 'track_event'.
   */
  readonly kind: 'track_event';
  /**
   * The URI of the track where the event is located.
   */
  readonly trackUri: string;
  /**
   * The ID of the selected event.
   */
  readonly eventId: number;
}

/**
 * Represents a selection of an entire track.
 */
export interface TrackSelection {
  /**
   * The kind of selection, always 'track'.
   */
  readonly kind: 'track';
  /**
   * The URI of the selected track.
   */
  readonly trackUri: string;
}

/**
 * Details about a track event.
 */
export interface TrackEventDetails {
  /**
   * The timestamp of the event. Required by the core.
   */
  readonly ts: time;

  /**
   * The duration of the event. Can be 0 for instant events or -1 for DNF
   * slices. Will be `undefined` if this selection has no duration (e.g.,
   * profile/counter samples).
   */
  readonly dur?: duration;
}

/**
 * Defines an area on the timeline.
 */
export interface Area {
  /**
   * The start timestamp of the area.
   */
  readonly start: time;
  /**
   * The end timestamp of the area.
   */
  readonly end: time;
  /**
   * An array of URIs of the tracks included in the area.
   */
  readonly trackUris: ReadonlyArray<string>;
}

/**
 * Represents a selection of an area on the timeline.
 */
export interface AreaSelection extends Area {
  /**
   * The kind of selection, always 'area'.
   */
  readonly kind: 'area';

  /**
   * This array contains the resolved Tracks from `Area.trackUris`. The
   * resolution is done by `SelectionManager` whenever a `kind='area'` selection
   * is performed.
   */
  readonly tracks: ReadonlyArray<Track>;
}

/**
 * Represents a selection of a note.
 */
export interface NoteSelection {
  /**
   * The kind of selection, always 'note'.
   */
  readonly kind: 'note';
  /**
   * The ID of the selected note.
   */
  readonly id: string;
}

/**
 * Represents an empty selection.
 */
export interface EmptySelection {
  /**
   * The kind of selection, always 'empty'.
   */
  readonly kind: 'empty';
}

/**
 * Resolves SQL events to track events.
 */
export interface SqlSelectionResolver {
  /**
   * The name of the SQL table to resolve.
   */
  readonly sqlTableName: string;
  /**
   * A callback function that resolves an event ID from a SQL table to a track
   * URI and event ID.
   * @param id The ID of the event in the SQL table.
   * @param sqlTable The name of the SQL table.
   * @returns A promise that resolves to an object containing `trackUri` and
   *   `eventId`, or `undefined` if not found.
   */
  callback(
    id: number,
    sqlTable: string,
  ): Promise<{readonly trackUri: string; readonly eventId: number} | undefined>;
}

/**
 * Manages the current selection state in the UI.
 */
export interface SelectionManager {
  /**
   * The current selection.
   */
  readonly selection: Selection;

  /**
   * Provides a list of registered area selection tabs.
   */
  readonly areaSelectionTabs: ReadonlyArray<AreaSelectionTab>;

  /**
   * Clears the current selection, selecting nothing.
   */
  clearSelection(): void;

  /**
   * Selects a track event.
   *
   * @param trackUri The URI of the track to select.
   * @param eventId The value of the event's ID column.
   * @param opts Additional options for the selection.
   */
  selectTrackEvent(
    trackUri: string,
    eventId: number,
    opts?: SelectionOpts,
  ): void;

  /**
   * Selects a track.
   *
   * @param trackUri The URI for the track to select.
   * @param opts Additional options for the selection.
   */
  selectTrack(trackUri: string, opts?: SelectionOpts): void;

  /**
   * Resolves events via a SQL table name and IDs.
   *
   * @param sqlTableName The name of the SQL table to resolve.
   * @param ids The IDs of the events in that table.
   * @returns A promise that resolves to an array of objects containing eventId
   *   and trackUri.
   */
  resolveSqlEvents(
    sqlTableName: string,
    ids: ReadonlyArray<number>,
  ): Promise<
    ReadonlyArray<{readonly eventId: number; readonly trackUri: string}>
  >;

  /**
   * Selects a track event via a SQL table name and ID.
   *
   * @param sqlTableName The name of the SQL table to resolve.
   * @param id The ID of the event in that table.
   * @param opts Additional options for the selection.
   */
  selectSqlEvent(sqlTableName: string, id: number, opts?: SelectionOpts): void;

  /**
   * Creates an area selection for the purposes of aggregation.
   *
   * @param args The area to select.
   * @param opts Additional options for the selection.
   */
  selectArea(args: Area, opts?: SelectionOpts): void;

  /**
   * Scrolls the timeline horizontally and vertically to reveal the currently
   * selected entity.
   */
  scrollToSelection(): void;

  /**
   * Returns the smallest time span that contains the currently selected entity.
   *
   * @returns The time span, if a timeline entity is selected, otherwise
   *   `undefined`.
   */
  getTimeSpanOfSelection(): TimeSpan | undefined;

  /**
   * Registers a new tab under the area selection details panel.
   * @param tab The area selection tab to register.
   */
  registerAreaSelectionTab(tab: AreaSelectionTab): void;
}

/**
 * Compare two area selections for equality. Returns true if the selections are
 * equivalent, false otherwise.
 * @param a The first area selection.
 * @param b The second area selection.
 * @returns `true` if the selections are equal, `false` otherwise.
 */
export function areaSelectionsEqual(
  a: AreaSelection,
  b: AreaSelection,
): boolean {
  if (a.start !== b.start) return false;
  if (a.end !== b.end) return false;
  if (!arrayEquals(a.trackUris, b.trackUris)) {
    return false;
  }
  return true;
}
