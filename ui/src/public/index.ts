// Copyright (C) 2022 The Android Open Source Project
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

import {Hotkey} from '../base/hotkeys';
import {duration, time} from '../base/time';
import {ColorScheme} from '../common/colorizer';
import {Selection} from '../common/state';
import {PanelSize} from '../frontend/panel';
import {Migrate, Store} from '../frontend/store';
import {EngineProxy} from '../trace_processor/engine';

export {createStore, Migrate, Store} from '../frontend/store';
export {EngineProxy} from '../trace_processor/engine';
export {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../trace_processor/query_result';
export {BottomTabToSCSAdapter} from './utils';

// This is a temporary fix until this is available in the plugin API.
export {
  createDebugSliceTrackActions,
  addDebugSliceTrack,
} from '../frontend/debug_tracks';

export interface Slice {
  // These properties are updated only once per query result when the Slice
  // object is created and don't change afterwards.
  readonly id: number;
  readonly startNsQ: time;
  readonly endNsQ: time;
  readonly durNsQ: duration;
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
  title: string;
  subTitle: string;
  colorScheme: ColorScheme;
  isHighlighted: boolean;
}

export interface Command {
  // A unique id for this command.
  id: string;
  // A human-friendly name for this command.
  name: string;
  // Callback is called when the command is invoked.
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  callback: (...args: any[]) => any;
  // Default hotkey for this command.
  // Note: this is just the default and may be changed by the user.
  // Examples:
  // - 'P'
  // - 'Shift+P'
  // - '!Mod+Shift+P'
  // See hotkeys.ts for guidance on hotkey syntax.
  defaultHotkey?: Hotkey;
}

export interface MetricVisualisation {
  // The name of the metric e.g. 'android_camera'
  metric: string;

  // A vega or vega-lite visualisation spec.
  // The data from the metric under path will be exposed as a
  // datasource named "metric" in Vega(-Lite)
  spec: string;

  // A path index into the metric.
  // For example if the metric returns the folowing protobuf:
  // {
  //   foo {
  //     bar {
  //       baz: { name: "a" }
  //       baz: { name: "b" }
  //       baz: { name: "c" }
  //     }
  //   }
  // }
  // That becomes the following json:
  // { "foo": { "bar": { "baz": [
  //  {"name": "a"},
  //  {"name": "b"},
  //  {"name": "c"},
  // ]}}}
  // And given path = ["foo", "bar", "baz"]
  // We extract:
  // [ {"name": "a"}, {"name": "b"}, {"name": "c"} ]
  // And pass that to the vega(-lite) visualisation.
  path: string[];
}

// This interface defines a context for a plugin, which is an object passed to
// most hooks within the plugin. It should be used to interact with Perfetto.
export interface PluginContext {
  // The unique ID for this plugin.
  readonly pluginId: string;

  // Register command against this plugin context.
  registerCommand(command: Command): void;

  // Run a command, optionally passing some args.
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  runCommand(id: string, ...args: any[]): any;

  // Control of the sidebar.
  sidebar: {
    // Show the sidebar.
    show(): void;

    // Hide the sidebar.
    hide(): void;

    // Returns true if the sidebar is visible.
    isVisible(): boolean;
  };
}

export interface TrackContext {
  // This track's key, used for making selections et al.
  trackKey: string;

  // Set of params passed in when the track was created.
  params: unknown;

  // Creates a new store overlaying the track instance's state object.
  // A migrate function must be passed to convert any existing state to a
  // compatible format.
  // When opening a fresh trace, the value of |init| will be undefined, and
  // state should be updated to an appropriate default value.
  // When loading a permalink, the value of |init| will be whatever was saved
  // when the permalink was shared, which might be from an old version of this
  // track.
  mountStore<State>(migrate: Migrate<State>): Store<State>;
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
   * Optional: Called when the track is first materialized on the timeline.
   * If this function returns a Promise, this promise is awaited before onUpdate
   * or onDestroy is called. Any calls made to these functions in the meantime
   * will be queued up and the hook will be called later once onCreate returns.
   * @param ctx Our track context object.
   */
  onCreate?(ctx: TrackContext): Promise<void>|void;

  /**
   * Optional: Called every render cycle while the track is visible, just before
   * render().
   * If this function returns a Promise, this promise is awaited before another
   * onUpdate is called or onDestroy is called.
   */
  onUpdate?(): Promise<void>|void;

  /**
   * Optional: Called when the track is no longer visible. Should be used to
   * clean up resources.
   * This function can return nothing or a promise. The promise is currently
   * ignored.
   */
  onDestroy?(): Promise<void>|void;

  render(ctx: CanvasRenderingContext2D, size: PanelSize): void;
  onFullRedraw?(): void;
  getSliceRect?(tStart: time, tEnd: time, depth: number): SliceRect|undefined;
  getHeight(): number;
  getTrackShellButtons?(): m.Children;
  onMouseMove?(position: {x: number, y: number}): void;
  onMouseClick?(position: {x: number, y: number}): boolean;
  onMouseOut?(): void;
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
}

// Tracks within track groups (usually corresponding to processes) are sorted.
// As we want to group all tracks related to a given thread together, we use
// two keys:
// - Primary key corresponds to a priority of a track block (all tracks related
//   to a given thread or a single track if it's not thread-associated).
// - Secondary key corresponds to a priority of a given thread-associated track
//   within its thread track block.
// Each track will have a sort key, which either a primary sort key
// (for non-thread tracks) or a tid and secondary sort key (mapping of tid to
// primary sort key is done independently).
export enum PrimaryTrackSortKey {
  DEBUG_TRACK,
  NULL_TRACK,
  PROCESS_SCHEDULING_TRACK,
  PROCESS_SUMMARY_TRACK,
  EXPECTED_FRAMES_SLICE_TRACK,
  ACTUAL_FRAMES_SLICE_TRACK,
  PERF_SAMPLES_PROFILE_TRACK,
  HEAP_PROFILE_TRACK,
  MAIN_THREAD,
  RENDER_THREAD,
  GPU_COMPLETION_THREAD,
  CHROME_IO_THREAD,
  CHROME_COMPOSITOR_THREAD,
  ORDINARY_THREAD,
  COUNTER_TRACK,
  ASYNC_SLICE_TRACK,
  ORDINARY_TRACK,
}

export interface SliceTrackColNames {
  ts: string;
  name: string;
  dur: string;
}

export interface DebugSliceTrackArgs {
  // Title of the track. If omitted a placeholder name will be chosen instead.
  trackName?: string;

  // Mapping definitions of the 'ts', 'dur', and 'name' columns.
  // By default, columns called ts, dur and name will be used.
  // If dur is assigned the value '0', all slices shall be instant events.
  columnMapping?: Partial<SliceTrackColNames>;

  // Any extra columns to be used as args.
  args?: string[];

  // Optional renaming of columns.
  columns?: string[];
}

export interface CounterTrackColNames {
  ts: string;
  value: string;
}

export interface DebugCounterTrackArgs {
  // Title of the track. If omitted a placeholder name will be chosen instead.
  trackName?: string;

  // Mapping definitions of the ts and value columns.
  columnMapping?: Partial<CounterTrackColNames>;
}

export interface Tab {
  hasContent?(): boolean;
  render(): m.Children;
  getTitle(): string;
}

export interface TabDescriptor {
  uri: string;  // TODO(stevegolton): Maybe optional for ephemeral tabs.
  content: Tab;
  isEphemeral?: boolean;  // Defaults false
  onHide?(): void;
  onShow?(): void;
}

export interface DetailsPanel {
  render(selection: Selection): m.Children;
  isLoading?(): boolean;
}

// Similar to PluginContext but with additional methods to operate on the
// currently loaded trace. Passed to trace-relevant hooks on a plugin instead of
// PluginContext.
export interface PluginContextTrace extends PluginContext {
  readonly engine: EngineProxy;

  // Control over the main timeline.
  timeline: {
    // Add a new track to the scrolling track section, returning the newly
    // created track key.
    addTrack(uri: string, displayName: string, params?: unknown): string;

    // Remove a single track from the timeline.
    removeTrack(key: string): void;

    // Pin a single track.
    pinTrack(key: string): void;

    // Unpin a single track.
    unpinTrack(key: string): void;

    // Pin all tracks that match a predicate.
    pinTracksByPredicate(predicate: TrackPredicate): void;

    // Unpin all tracks that match a predicate.
    unpinTracksByPredicate(predicate: TrackPredicate): void;

    // Remove all tracks that match a predicate.
    removeTracksByPredicate(predicate: TrackPredicate): void;

    // Expand all groups that match a predicate.
    expandGroupsByPredicate(predicate: GroupPredicate): void;

    // Collapse all groups that match a predicate.
    collapseGroupsByPredicate(predicate: GroupPredicate): void;

    // Retrieve a list of tracks on the timeline.
    tracks: TrackRef[];

    // Bring a timestamp into view.
    panToTimestamp(ts: time): void;
  }

  // Control over the bottom details pane.
  tabs: {
    // Creates a new tab running the provided query.
    openQuery(query: string, title: string): void;

    // Add a tab to the tab bar (if not already) and focus it.
    showTab(uri: string): void;

    // Remove a tab from the tab bar.
    hideTab(uri: string): void;
  }

  // Register a new track against a unique key known as a URI.
  // Once a track is registered it can be referenced multiple times on the
  // timeline with different params to allow customising each instance.
  registerTrack(trackDesc: TrackDescriptor): void;

  // Add a new entry to the pool of default tracks. Default tracks are a list
  // of track references that describe the list of tracks that should be added
  // to the main timeline on startup.
  // Default tracks are only used when a trace is first loaded, not when
  // loading from a permalink, where the existing list of tracks from the
  // shared state is used instead.
  addDefaultTrack(track: TrackRef): void;

  // Simultaneously register a track and add it as a default track in one go.
  // This is simply a helper which calls registerTrack() and addDefaultTrack()
  // with the same URI.
  registerStaticTrack(track: TrackDescriptor&TrackRef): void;

  // Register a new tab for this plugin. Will be unregistered when the plugin
  // is deactivated or when the trace is unloaded.
  registerTab(tab: TabDescriptor): void;

  // Suggest that a tab should be shown immediately.
  addDefaultTab(uri: string): void;

  // Register a hook into the current selection tab rendering logic that allows
  // customization of the current selection tab content.
  registerDetailsPanel(sel: DetailsPanel): void;

  // Create a store mounted over the top of this plugin's persistent state.
  mountStore<T>(migrate: Migrate<T>): Store<T>;
}

export interface Plugin {
  // Lifecycle methods.
  onActivate(ctx: PluginContext): void;
  onTraceLoad?(ctx: PluginContextTrace): Promise<void>;
  onTraceUnload?(ctx: PluginContextTrace): Promise<void>;
  onDeactivate?(ctx: PluginContext): void;

  // Extension points.
  metricVisualisations?(ctx: PluginContext): MetricVisualisation[];
}

// This interface defines what a plugin factory should look like.
// This can be defined in the plugin class definition by defining a constructor
// and the relevant static methods:
// E.g.
// class MyPlugin implements TracePlugin<MyState> {
//   migrate(initialState: unknown): MyState {...}
//   constructor(store: Store<MyState>, engine: EngineProxy) {...}
//   ... methods from the TracePlugin interface go here ...
// }
// ... which can then be passed around by class i.e. MyPlugin
export interface PluginClass {
  // Instantiate the plugin.
  new(): Plugin;
}

// Describes a reference to a registered track.
export interface TrackRef {
  // URI of the registered track.
  uri: string;

  // A human readable name for this track - displayed in the track shell.
  displayName: string;

  // Optional: An opaque object used to customize this instance of the track.
  params?: unknown;

  // Optional: Used to define default sort order for new traces.
  // Note: This will be deprecated soon in favour of tags & sort rules.
  sortKey?: PrimaryTrackSortKey;

  // Optional: Add tracks to a group with this name.
  groupName?: string;
}

// A predicate for selecting a subset of tracks.
export type TrackPredicate = (info: TrackTags) => boolean;

// Describes a reference to a group of tracks.
export interface GroupRef {
  // A human readable name for this track group.
  displayName: string;

  // True if the track is open else false.
  collapsed: boolean;
}

// A predicate for selecting a subset of groups.
export type GroupPredicate = (info: GroupRef) => boolean;

interface WellKnownTrackTags {
  // A human readable name for this specific track.
  name: string;

  // Controls whether to show the "metric" chip.
  metric: boolean;

  // Controls whether to show the "debuggable" chip.
  debuggable: boolean;
}

// An set of key/value pairs describing a given track. These are used for
// selecting tracks to pin/unpin, diplsaying "chips" in the track shell, and
// (in future) the sorting and grouping of tracks.
// We define a handful of well known fields, and the rest are arbitrary key-
// value pairs.
export type TrackTags = Partial<WellKnownTrackTags>&{
  // There may be arbitrary other key/value pairs.
  [key: string]: string|number|boolean|undefined;
}

// Plugins can be passed as class refs, factory functions, or concrete plugin
// implementations.
export type PluginFactory = PluginClass|Plugin|(() => Plugin);

export interface PluginDescriptor {
  // A unique string for your plugin. To ensure the name is unique you
  // may wish to use a URL with reversed components in the manner of
  // Java package names.
  pluginId: string;

  // The plugin factory used to instantiate the plugin object, or if this is
  // an actual plugin implementation, it's just used as-is.
  plugin: PluginFactory;
}
