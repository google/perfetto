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
import {duration, time} from '../base/time';
import {Optional} from '../base/utils';
import {UntypedEventSet} from '../core/event_set';
import {LegacySelection, Selection} from '../core/selection_manager';
import {Size} from '../base/geom';

export interface TrackContext {
  // This track's key, used for making selections et al.
  trackKey: string;
}

// A definition of a track, including a renderer implementation and metadata.
export interface TrackDescriptor {
  // A unique identifier for this track.
  uri: string;

  // A factory function returning a new track instance.
  trackFactory: (ctx: TrackContext) => Track;

  // The track "kind", used by various subsystems e.g. aggregation controllers.
  // This is where "XXX_TRACK_KIND" values should be placed.
  // TODO(stevegolton): This will be deprecated once we handle group selections
  // in a more generic way - i.e. EventSet.
  kind?: string;

  // Optional: list of track IDs represented by this trace.
  // This list is used for participation in track indexing by track ID.
  // This index is used by various subsystems to find links between tracks based
  // on the track IDs used by trace processor.
  trackIds?: number[];

  // Optional: The CPU number associated with this track.
  cpu?: number;

  // Optional: The UTID associated with this track.
  utid?: number;

  // Optional: The UPID associated with this track.
  upid?: number;

  // Optional: A list of tags used for sorting, grouping and "chips".
  tags?: TrackTags;

  // Placeholder - presently unused.
  displayName?: string;

  // Optional: method to look up the start and duration of an event on this track
  getEventBounds?: (id: number) => Promise<Optional<{ts: time; dur: duration}>>;

  // Optional: A details panel to use when this track is selected.
  detailsPanel?: TrackSelectionDetailsPanel;
}

export interface LegacyDetailsPanel {
  render(selection: LegacySelection): m.Children;
  isLoading?(): boolean;
}

export interface DetailsPanel {
  render(selection: Selection): m.Children;
  isLoading?(): boolean;
}

export interface TrackSelectionDetailsPanel {
  render(id: number): m.Children;
  isLoading?(): boolean;
}

export interface SliceRect {
  left: number;
  width: number;
  top: number;
  height: number;
  visible: boolean;
}

export interface Track {
  /**
   * Optional: Called once before onUpdate is first called.
   *
   * If this function returns a Promise, this promise is awaited before onUpdate
   * or onDestroy is called. Any calls made to these functions in the meantime
   * will be queued up and the hook will be called later once onCreate returns.
   *
   * Exactly when this hook is called is left purposely undefined. The only
   * guarantee is that it will be called once before onUpdate is first called.
   *
   * @param ctx Our track context object.
   */
  onCreate?(ctx: TrackContext): Promise<void> | void;

  /**
   * Optional: Called every render cycle while the track is visible, just before
   * render().
   * If this function returns a Promise, this promise is awaited before another
   * onUpdate is called or onDestroy is called.
   */
  onUpdate?(): Promise<void> | void;

  /**
   * Optional: Called when the track is no longer visible. Should be used to
   * clean up resources.
   * This function can return nothing or a promise. The promise is currently
   * ignored.
   */
  onDestroy?(): Promise<void> | void;

  render(ctx: CanvasRenderingContext2D, size: Size): void;
  onFullRedraw?(): void;
  getSliceRect?(tStart: time, tEnd: time, depth: number): SliceRect | undefined;
  getHeight(): number;
  getTrackShellButtons?(): m.Children;
  onMouseMove?(position: {x: number; y: number}): void;
  onMouseClick?(position: {x: number; y: number}): boolean;
  onMouseOut?(): void;

  /**
   * Optional: Get the event set that represents this track's data.
   */
  getEventSet?(): UntypedEventSet;
}

// An set of key/value pairs describing a given track. These are used for
// selecting tracks to pin/unpin, diplsaying "chips" in the track shell, and
// (in future) the sorting and grouping of tracks.
// We define a handful of well known fields, and the rest are arbitrary key-
// value pairs.
export type TrackTags = Partial<WellKnownTrackTags> & {
  // There may be arbitrary other key/value pairs.
  [key: string]: string | number | boolean | undefined;
};

interface WellKnownTrackTags {
  // A human readable name for this specific track.
  name: string;

  // Controls whether to show the "metric" chip.
  metric: boolean;

  // Controls whether to show the "debuggable" chip.
  debuggable: boolean;

  // Groupname of the track
  groupName: string;
}
