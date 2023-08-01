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

import {Disposable} from '../base/disposable';
import {EngineProxy} from '../common/engine';
import {TrackControllerFactory} from '../controller/track_controller';
import {Store} from '../frontend/store';
import {TrackCreator} from '../frontend/track';

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
}

export interface Command {
  // A unique id for this command.
  id: string;
  // A friendly human name for the command.
  name: string;
  // Callback is called when the command is invoked.
  callback: (...args: any[]) => void;
}

// All trace plugins must implement this interface.
export interface TracePlugin extends Disposable {
  commands?: () => Command[];

  // Called any time a trace is loaded. Plugins should return all
  // potential tracks. Zero or more of the provided tracks may be
  // instantiated depending on the users choices.
  tracks?: () => Promise<TrackInfo[]>;
}

// This interface defines what a plugin factory should look like.
// This can be defined in the plugin class definition by defining a constructor
// and the relevant static methods:
// E.g.
// class MyPlugin implements TracePlugin<MyState> {
//   static migrate(initialState: unknown): MyState {...}
//   constructor(store: Store<MyState>, engine: EngineProxy) {...}
//   ... methods from the TracePlugin interface go here ...
// }
// ... which can then be passed around by class i.e. MyPlugin
export interface TracePluginFactory<StateT> {
  // Function to migrate the persistent state. Called before new().
  migrate(initialState: unknown): StateT;

  // Instantiate the plugin.
  new(store: Store<StateT>, engine: EngineProxy, viewer: Viewer): TracePlugin;
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

// The public API plugins use to extend the UI. This is passed to each
// plugin via the exposed 'activate' function.
export interface PluginContext {
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

  // Register a new plugin factory for a plugin whose lifecycle in linked to
  // that of the trace.
  registerTracePluginFactory<T>(pluginFactory: TracePluginFactory<T>): void;
}

export interface PluginInfo {
  // A unique string for your plugin. To ensure the name is unique you
  // may wish to use a URL with reversed components in the manner of
  // Java package names.
  pluginId: string;

  // This function is called when the plugin is loaded. Generally this
  // is called at most once shortly after the UI is loaded. However in
  // some situations it can be called multiple times - for example
  // when the user is toggling plugins on/off.
  activate: (ctx: PluginContext) => void;
}
