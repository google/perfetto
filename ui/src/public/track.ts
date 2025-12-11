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
import {CanvasColors} from './canvas_colors';
import {z} from 'zod';

/**
 * Represents a snap point for the snap-to-boundaries feature.
 * When dragging selection boundaries, the cursor can snap to these points
 * to enable precise measurement of time intervals.
 */
export interface SnapPoint {
  /** The timestamp to snap to */
  time: time;
}

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

  /**
   * Semantic colors which can vary depending on the current theme.
   */
  readonly colors: CanvasColors;
}

// A definition of a track, including a renderer implementation and metadata.
export interface Track {
  // A unique identifier for this track.
  readonly uri: string;

  // Describes how to render the track.
  readonly renderer: TrackRenderer;

  // Optional: A human readable description of the track. This can be a simple
  // string or a render function that returns Mithril vnodes.
  readonly description?: string | (() => m.Children);

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

/**
 * A descriptor for a track setting, which describes the setting's metadata and
 * how to render a control for it. This is separate from the track setting
 * itself because multiple tracks (which will need different TrackSetting
 * instances) can share the same descriptor, which would make them editable in
 * bulk.
 *
 * A lot of the fields in this interface are currently unused, but they will be
 * used in the future when track serialization is implemented.
 */
export interface TrackSettingDescriptor<T> {
  // A unique identifier for this setting. Will be used to store the serialized
  // value for this setting. Currently unused.
  readonly id: string;

  // A human readable name for this setting. This is displayed in the settings
  // menu unless overridden.
  readonly name: string;

  // A human readable description for this setting. Currently unused, but good
  // practice to require this in order to document what a setting does and is
  // used for.
  readonly description: string;

  // A Zod schema describing the setting's value type which is used to infer the
  // automatic settings menu type and options, and will be used for
  // serialization and deserialization.
  readonly schema: z.ZodType<T>;

  // The default value for this setting. This will be used to render a 'reset'
  // button in the render menu, and possibly as a fallback if parsing fails when
  // we add serialization. Currently unused.
  readonly defaultValue: T;

  // An optional function used to render a control for this setting. This
  // describes what the control looks like in the settings menu on the track and
  // also the bulk settings menu when multiple tracks are selected. If omitted,
  // a control will be automatically generated based on the schema and name.
  render?(setter: (value: T) => void, values: ReadonlyArray<T>): m.Children;
}

/**
 * A setting that can be changed by the user that affects how the track is
 * rendered or behaves. References a TrackSettingDescriptor which describes the
 * setting's metadata and how to render a control for it.
 */
export interface TrackSetting<T> {
  readonly descriptor: TrackSettingDescriptor<T>;
  getValue: () => T;
  setValue(newValue: T): void;
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
   * An optional list of settings that can be modified externally to affect how
   * the track is rendered or behaves.
   *
   * These settings are user-editable and are shown in the track settings menu.
   * They can also be modified in bulk if multiple tracks share the same setting
   * descriptor.
   *
   * Note: The core does not yet provide a mechanism to persist these settings
   * to permalinks.
   */
  readonly settings?: ReadonlyArray<TrackSetting<unknown>>;

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
  getHeight?(): number;
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

  /**
   * Optional: Find the nearest snap point to the given time.
   * Returns undefined if no snap point is within threshold.
   *
   * This method is used by the snap-to-boundaries feature to enable precise
   * measurement of time intervals. When dragging selection boundaries, the
   * cursor can snap to these points.
   *
   * @param targetTime - Target time to snap from
   * @param thresholdPx - Maximum pixel distance to snap
   * @param timescale - For converting between time and pixels
   * @returns The nearest snap point, or undefined if none within threshold
   */
  getSnapPoint?(
    targetTime: time,
    thresholdPx: number,
    timescale: TimeScale,
  ): SnapPoint | undefined;
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
  // The track "kinds", are by various subsystems e.g. aggregation controllers
  // in order to select tracks to operate on. A good analogy is how CSS
  // selectors can match elements using their class list.
  kinds: ReadonlyArray<string>;

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
  readonly count: number;
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
    theme: CanvasColors,
  ): void;
}
