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
import {Size2D, VerticalBounds} from '../base/geom';
import {TimeScale} from '../base/time_scale';
import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {ColorScheme} from '../base/color_scheme';
import {TrackEventDetailsPanel} from './details_panel';
import {TrackEventDetails, TrackEventSelection} from './selection';
import {SourceDataset} from '../trace_processor/dataset';
import {TrackNode} from './workspace';

export interface TrackFilterCriteria {
  readonly name: string;

  // Run on each node to work out whether it satisfies the selected filter
  // option.
  readonly predicate: (track: TrackNode, filterOption: string) => boolean;

  // The list of possible filter options.
  readonly options: ReadonlyArray<{key: string; label: string}>;
}

export interface TrackManager {
  /**
   * Register a new track against a unique key known as a URI. The track is not
   * shown by default and callers need to either manually add it to a
   * Workspace or use registerTrackAndShowOnTraceLoad() below.
   */
  registerTrack(track: Track): void;

  findTrack(
    predicate: (track: Track) => boolean | undefined,
  ): Track | undefined;

  getAllTracks(): Track[];

  getTrack(uri: string): Track | undefined;

  /**
   * Register a track filter criteria, which can be used by end users to control
   * the list of tracks they see in workspaces. These criteria can provide more
   * power to the user compared to e.g. purely filtering by name.
   */
  registerTrackFilterCriteria(filter: TrackFilterCriteria): void;

  /**
   * Register a timeline overlay renderer.
   *
   * Overlays are rendered on top of all tracks in the timeline view and can be
   * used to draw annotations that span multiple tracks, such as flow arrows or
   * vertical lines marking specific events.
   */
  registerOverlay(overlay: Overlay): void;
}

export interface TrackContext {
  // This track's URI, used for making selections et al.
  readonly trackUri: string;
}

/**
 * Contextual information about the track passed to track lifecycle hooks &
 * render hooks with additional information about the timeline/canvas.
 */
export interface TrackRenderContext extends TrackContext {
  /**
   * The time span of the visible window.
   */
  readonly visibleWindow: HighPrecisionTimeSpan;

  /**
   * The dimensions of the track on the canvas in pixels.
   */
  readonly size: Size2D;

  /**
   * Suggested data resolution.
   *
   * This number is the number of time units that corresponds to 1 pixel on the
   * screen, rounded down to the nearest power of 2. The minimum value is 1.
   *
   * It's up to the track whether it would like to use this resolution or
   * calculate their own based on the timespan and the track dimensions.
   */
  readonly resolution: duration;

  /**
   * Canvas context used for rendering.
   */
  readonly ctx: CanvasRenderingContext2D;

  /**
   * A time scale used for translating between pixels and time.
   */
  readonly timescale: TimeScale;
}

// A definition of a track, including a renderer implementation and metadata.
export interface Track {
  // A unique identifier for this track.
  readonly uri: string;

  // Describes how to render the track.
  readonly track: TrackRenderer;

  // Human readable title. Always displayed.
  readonly title: string;

  // Optional: A human readable description of the track.
  readonly description?: string;

  // Optional: Human readable subtitle. Sometimes displayed if there is room.
  readonly subtitle?: string;

  // Optional: A list of tags which provide additional metadata about the track.
  // Used mainly for legacy purposes that predate dataset.
  readonly tags?: TrackTags;

  // Optional: A list of strings which are displayed as "chips" in the track
  // shell.
  readonly chips?: ReadonlyArray<string>;

  // Filled in by the core.
  readonly pluginId?: string;
}

/**
 * Contextual information passed to mouse events.
 */
export interface TrackMouseEvent {
  /**
   * X coordinate of the mouse event w.r.t. the top-left of the track.
   */
  readonly x: number;

  /**
   * Y coordinate of the mouse event w.r.t the top-left of the track.
   */
  readonly y: number;

  /**
   * A time scale used for translating between pixels and time.
   */
  readonly timescale: TimeScale;
}

export interface TrackRenderer {
  /**
   * Describes which root table the events on this track come from. This is
   * mainly for use by flows (before they get refactored to be more generic) and
   * will be used by the SQL table resolver mechanism along with dataset.
   * TODO(stevegolton): Maybe move this onto dataset directly?
   */
  readonly rootTableName?: string;

  /**
   * Optional lifecycle hook called on the first render cycle. Should be used to
   * create any required resources.
   *
   * These lifecycle hooks are asynchronous, but they are run synchronously,
   * meaning that perfetto will wait for each one to complete before calling the
   * next one, so the user doesn't have to serialize these calls manually.
   *
   * Exactly when this hook is called is left purposely undefined. The only
   * guarantee is that it will be called exactly once before the first call to
   * onUpdate().
   *
   * Note: On the first render cycle, both onCreate and onUpdate are called one
   * after another.
   */
  onCreate?(ctx: TrackContext): Promise<void>;

  /**
   * Optional lifecycle hook called on every render cycle.
   *
   * The track should inspect things like the visible window, track size, and
   * resolution to work out whether any data needs to be reloaded based on these
   * properties and perform a reload.
   */
  onUpdate?(ctx: TrackRenderContext): Promise<void>;

  /**
   * Optional lifecycle hook called when the track is no longer visible. Should
   * be used to clear up any resources.
   */
  onDestroy?(): Promise<void>;

  /**
   * Required method used to render the track's content to the canvas, called
   * synchronously on every render cycle.
   */
  render(ctx: TrackRenderContext): void;
  onFullRedraw?(): void;

  /**
   * Return the vertical bounds (top & bottom) of a slice were it to be rendered
   * at a specific depth, given the slice height and padding/spacing that this
   * track uses.
   */
  getSliceVerticalBounds?(depth: number): VerticalBounds | undefined;
  getHeight(): number;
  getTrackShellButtons?(): m.Children;
  onMouseMove?(event: TrackMouseEvent): void;
  onMouseClick?(event: TrackMouseEvent): boolean;
  onMouseOut?(): void;

  /**
   * Optional: Returns a dataset that represents the events displayed on this
   * track.
   */
  getDataset?(): SourceDataset | undefined;

  /**
   * Optional: Get details of a track event given by eventId on this track.
   */
  getSelectionDetails?(eventId: number): Promise<TrackEventDetails | undefined>;

  // Optional: A factory that returns a details panel object for a given track
  // event selection. This is called each time the selection is changed (and the
  // selection is relevant to this track).
  detailsPanel?(sel: TrackEventSelection): TrackEventDetailsPanel | undefined;

  // Optional: Returns tooltip content if available. If the return value is
  // falsy, no tooltip is rendered.
  renderTooltip?(): m.Children;
}

// An set of key/value pairs describing a given track. These are used for
// selecting tracks to pin/unpin, diplsaying "chips" in the track shell, and
// (in future) the sorting and grouping of tracks.
// We define a handful of well known fields, and the rest are arbitrary key-
// value pairs.
export type TrackTags = Partial<WellKnownTrackTags> & {
  // There may be arbitrary other key/value pairs.
  [key: string]:
    | undefined
    | string
    | number
    | boolean
    | ReadonlyArray<string>
    | ReadonlyArray<number>;
};

interface WellKnownTrackTags {
  // The track "kind", used by various subsystems e.g. aggregation controllers.
  // This is where "XXX_TRACK_KIND" values should be placed.
  // TODO(stevegolton): This will be deprecated once we handle group selections
  // in a more generic way - i.e. EventSet.
  kind: string;

  // Optional: list of track IDs represented by this trace.
  // This list is used for participation in track indexing by track ID.
  // This index is used by various subsystems to find links between tracks based
  // on the track IDs used by trace processor.
  trackIds: ReadonlyArray<number>;

  // Optional: The CPU number associated with this track.
  cpu: number;

  // Optional: The UTID associated with this track.
  utid: number;

  // Optional: The UPID associated with this track.
  upid: number;

  // Used for sorting and grouping
  scope: string;

  // Group name, used as a hint to ask track decider to put this in a group
  groupName: string;

  // Track type, used for filtering
  type: string;
}

export interface Slice {
  // These properties are updated only once per query result when the Slice
  // object is created and don't change afterwards.
  readonly id: number;
  readonly startNs: time;
  readonly endNs: time;
  readonly durNs: duration;
  readonly ts: time;
  readonly dur: duration;
  readonly depth: number;
  readonly flags: number;

  // Each slice can represent some extra numerical information by rendering a
  // portion of the slice with a lighter tint.
  // |fillRatio\ describes the ratio of the normal area to the tinted area
  // width of the slice, normalized between 0.0 -> 1.0.
  // 0.0 means the whole slice is tinted.
  // 1.0 means none of the slice is tinted.
  // E.g. If |fillRatio| = 0.65 the slice will be rendered like this:
  // [############|*******]
  // ^------------^-------^
  //     Normal     Light
  readonly fillRatio: number;

  // These can be changed by the Impl.
  title?: string;
  subTitle: string;
  colorScheme: ColorScheme;
  isHighlighted: boolean;
}

/**
 * Contains a track and it's top and bottom coordinates in the timeline.
 */
export interface TrackBounds {
  readonly node: TrackNode;
  readonly verticalBounds: VerticalBounds;
}

export interface Overlay {
  render(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
    tracks: ReadonlyArray<TrackBounds>,
  ): void;
}
