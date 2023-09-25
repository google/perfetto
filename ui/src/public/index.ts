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
import {duration, Span, time} from '../base/time';
import {EngineProxy} from '../common/engine';
import {TrackControllerFactory} from '../controller/track_controller';
import {Store} from '../frontend/store';
import {PxSpan, TimeScale} from '../frontend/time_scale';
import {SliceRect, TrackCreator} from '../frontend/track';
import {TrackButtonAttrs} from '../frontend/track_panel';

export {EngineProxy} from '../common/engine';
export {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../common/query_result';
export {Store} from '../frontend/store';


// An imperative API for plugins to change the UI.
export interface Viewer {
  // Control of the sidebar.
  sidebar: {
    // Show the sidebar.
    show(): void;
    // Hide the sidebar.
    hide(): void;
    // Returns true if the sidebar is visble.
    isVisible(): boolean;
  }

  // Tracks
  tracks: {
    pin(predicate: TrackPredicate): void;
    unpin(predicate: TrackPredicate): void;
  }

  // Control over the bottom details pane.
  tabs: {
    // Creates a new tab running the provided query.
    openQuery(query: string, title: string): void;
  }

  commands: {run(name: string, ...args: any[]): void;}
}

export interface Command {
  // A unique id for this command.
  id: string;
  // A human-friendly name for this command.
  name: string;
  // Callback is called when the command is invoked.
  callback: (...args: any[]) => void;
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
  readonly viewer: Viewer;

  // DEPRECATED. In prior versions of the UI tracks were split into a
  // 'TrackController' and a 'Track'. In more recent versions of the UI
  // the functionality of |TrackController| has been merged into Track so
  // |TrackController|s are not necessary in new code.
  registerTrackController(track: TrackControllerFactory): void;

  // Register a track factory. The core UI invokes |TrackCreator| to
  // construct tracks discovered by invoking |TrackProvider|s.
  // The split between 'construction' and 'discovery' allows
  // plugins to reuse common tracks for new data. For example: the
  // dev.perfetto.AndroidGpu plugin could register a TrackProvider
  // which returns GPU counter tracks. The counter track factory itself
  // could be registered in dev.perfetto.CounterTrack - a whole
  // different plugin.
  registerTrack(track: TrackCreator): void;

  // Add a command.
  addCommand(command: Command): void;
}

export interface TrackContext {
  // A unique ID for the instance of this track.
  trackInstanceId: string;
}

// TODO(stevegolton): Rename `Track` to `BaseTrack` (or similar) and rename this
// interface to `Track`.
export interface TrackLike {
  onCreate(): void;
  render(ctx: CanvasRenderingContext2D): void;
  onFullRedraw(): void;
  getSliceRect(
      visibleTimeScale: TimeScale, visibleWindow: Span<time, duration>,
      windowSpan: PxSpan, tStart: time, tEnd: time, depth: number): SliceRect
      |undefined;
  getHeight(): number;
  getTrackShellButtons(): Array<m.Vnode<TrackButtonAttrs>>;
  getContextMenu(): m.Vnode<any>|null;
  onMouseMove(position: {x: number, y: number}): void;
  onMouseClick(position: {x: number, y: number}): boolean;
  onMouseOut(): void;
  onDestroy(): void;
}

export interface PluginTrackInfo {
  // A unique identifier for the track. This must be unique within all tracks.
  uri: string;

  // A human friendly name for this track. Used when displaying the list of
  // tracks to the user. E.g. when adding a new track to the workspace.
  displayName: string;

  // A factory function returning the track object.
  trackFactory: (ctx: TrackContext) => TrackLike;

  // A list of tags used for sorting and grouping.
  tags?: TrackTags;
}

// Similar to PluginContext but with additional properties to operate on the
// currently loaded trace. Passed to trace-relevant hooks instead of
// PluginContext.
export interface TracePluginContext<T = undefined> extends PluginContext {
  readonly engine: EngineProxy;
  readonly store: Store<T>;

  // Add a new track from this plugin. The track is just made available here,
  // it's not automatically shown until it's added to a workspace.
  addTrack(trackDetails: PluginTrackInfo): void;
}

export interface BasePlugin<State> {
  // Lifecycle methods.
  onActivate(ctx: PluginContext): void;
  onTraceLoad?(ctx: TracePluginContext<State>): Promise<void>;
  onTraceUnload?(ctx: TracePluginContext<State>): Promise<void>;
  onDeactivate?(ctx: PluginContext): void;

  // Extension points.
  metricVisualisations?(ctx: PluginContext): MetricVisualisation[];
  findPotentialTracks?(ctx: TracePluginContext<State>): Promise<TrackInfo[]>;
}

export interface StatefulPlugin<State> extends BasePlugin<State> {
  // Function to migrate the persistent state.
  migrate(initialState: unknown): State;
}

// Generic interface all plugins must implement.
// If a state type is passed, the plugin must implement migrate(). Otherwise if
// the state type is omitted, migrate need not be defined.
export type Plugin<State = undefined> =
    State extends undefined ? BasePlugin<State>: StatefulPlugin<State>;

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
export interface PluginClass<T> {
  // Instantiate the plugin.
  new(): Plugin<T>;
}

export interface TrackInfo {
  // The id of this 'type' of track. This id is used to select the
  // correct |TrackCreator| to construct the track.
  trackKind: string;

  // A human readable name for this specific track. It will normally be
  // displayed on the left-hand-side of the track.
  name: string;

  // An opaque config for the track.
  config: {};
}

// A predicate for selecting a groups of tracks.
export type TrackPredicate = (info: TrackTags) => boolean;

// An set of key/value pairs describing a given track. These
// are used for selecting tracks to pin/unpin and (in future) the
// sorting and grouping of tracks. The values are always strings.
export interface TrackTags {
  // A human readable name for this specific track.
  name?: string;

  // There may be arbitrary other key/value pairs.
  [key: string]: string|undefined;
}

// Plugins can be passed as class refs, factory functions, or concrete plugin
// implementations.
export type PluginFactory<T> = PluginClass<T>|Plugin<T>|(() => Plugin<T>);

export interface PluginInfo<T = undefined> {
  // A unique string for your plugin. To ensure the name is unique you
  // may wish to use a URL with reversed components in the manner of
  // Java package names.
  pluginId: string;

  // The plugin factory used to instantiate the plugin object, or if this is
  // an actual plugin implementation, it's just used as-is.
  plugin: PluginFactory<T>;
}
