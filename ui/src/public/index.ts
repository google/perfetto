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
import {TimeSpan, duration, time} from '../base/time';
import {Migrate, Store} from '../base/store';
import {ColorScheme} from '../core/colorizer';
import {PrimaryTrackSortKey} from '../common/state';
import {Engine} from '../trace_processor/engine';
import {PromptOption} from '../frontend/omnibox_manager';
import {LegacyDetailsPanel, TrackDescriptor} from './tracks';
import {TraceContext} from '../frontend/trace_context';

export {Engine} from '../trace_processor/engine';
export {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../trace_processor/query_result';
export {BottomTabToSCSAdapter} from './utils';
export {createStore, Migrate, Store} from '../base/store';
export {PromptOption} from '../frontend/omnibox_manager';
export {PrimaryTrackSortKey} from '../common/state';

export {addDebugSliceTrack} from '../frontend/debug_tracks/debug_tracks';
export * from '../core/track_kinds';
export {
  TrackDescriptor,
  Track,
  TrackContext,
  TrackTags,
  SliceRect,
  DetailsPanel,
  LegacyDetailsPanel,
  TrackSelectionDetailsPanel,
} from './tracks';

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

export interface SidebarMenuItem {
  readonly commandId: string;
  readonly group:
    | 'navigation'
    | 'current_trace'
    | 'convert_trace'
    | 'example_traces'
    | 'support';
  when?(): boolean;
  readonly icon: string;
  readonly priority?: number;
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

  // Adds a new menu item to the sidebar.
  // All entries must map to a command. This will allow the shortcut and
  // optional shortcut to be displayed on the UI.
  addSidebarMenuItem(menuItem: SidebarMenuItem): void;
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
  render(): m.Children;
  getTitle(): string;
}

export interface TabDescriptor {
  uri: string; // TODO(stevegolton): Maybe optional for ephemeral tabs.
  content: Tab;
  isEphemeral?: boolean; // Defaults false
  onHide?(): void;
  onShow?(): void;
}

// Similar to PluginContext but with additional methods to operate on the
// currently loaded trace. Passed to trace-relevant hooks on a plugin instead of
// PluginContext.
export interface PluginContextTrace extends PluginContext {
  readonly engine: Engine;

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

    // Move the viewport
    setViewportTime(start: time, end: time): void;

    // A span representing the current viewport location
    readonly viewport: TimeSpan;
  };

  // Control over the bottom details pane.
  tabs: {
    // Creates a new tab running the provided query.
    openQuery(query: string, title: string): void;

    // Add a tab to the tab bar (if not already) and focus it.
    showTab(uri: string): void;

    // Remove a tab from the tab bar.
    hideTab(uri: string): void;
  };

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
  registerStaticTrack(track: TrackDescriptor & TrackRef): void;

  // Register a new tab for this plugin. Will be unregistered when the plugin
  // is deactivated or when the trace is unloaded.
  registerTab(tab: TabDescriptor): void;

  // Suggest that a tab should be shown immediately.
  addDefaultTab(uri: string): void;

  // Register a hook into the current selection tab rendering logic that allows
  // customization of the current selection tab content.
  registerDetailsPanel(sel: LegacyDetailsPanel): void;

  // Create a store mounted over the top of this plugin's persistent state.
  mountStore<T>(migrate: Migrate<T>): Store<T>;

  readonly trace: TraceContext;

  // When the trace is opened via postMessage deep-linking, returns the sub-set
  // of postMessageData.pluginArgs[pluginId] for the current plugin. If not
  // present returns undefined.
  readonly openerPluginArgs?: {[key: string]: unknown};

  prompt(text: string, options?: PromptOption[]): Promise<string>;
}

export interface Plugin {
  // Lifecycle methods.
  onActivate?(ctx: PluginContext): void;
  onTraceLoad?(ctx: PluginContextTrace): Promise<void>;
  onTraceReady?(ctx: PluginContextTrace): Promise<void>;
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
  new (): Plugin;
}

// Describes a reference to a registered track.
export interface TrackRef {
  // URI of the registered track.
  readonly uri: string;

  // A human readable name for this track - displayed in the track shell.
  readonly title: string;

  // Optional: Used to define default sort order for new traces.
  // Note: This will be deprecated soon in favour of tags & sort rules.
  readonly sortKey?: PrimaryTrackSortKey;

  // Optional: Add tracks to a group with this name.
  readonly groupName?: string;

  // Optional: Track key
  readonly key?: string;

  // Optional: Whether the track is pinned
  readonly isPinned?: boolean;
}

// A predicate for selecting a subset of tracks.
export type TrackPredicate = (info: TrackDescriptor) => boolean;

// Describes a reference to a group of tracks.
export interface GroupRef {
  // A human readable name for this track group.
  displayName: string;

  // True if the track is open else false.
  collapsed: boolean;
}

// A predicate for selecting a subset of groups.
export type GroupPredicate = (info: GroupRef) => boolean;

// Plugins can be class refs or concrete plugin implementations.
export type PluginFactory = PluginClass | Plugin;

export interface PluginDescriptor {
  // A unique string for your plugin. To ensure the name is unique you
  // may wish to use a URL with reversed components in the manner of
  // Java package names.
  pluginId: string;

  // The plugin factory used to instantiate the plugin object, or if this is
  // an actual plugin implementation, it's just used as-is.
  plugin: PluginFactory;
}
