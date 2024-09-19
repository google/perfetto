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

import {Trace} from './trace';
import {App} from './app';

// TODO(primiano): I think we should re-think the plugins lifecycle API. Having
// onTraceUnload and on(another)TraceLoad on the same object is too brittle.
// What is going to happen is that plugins will mix the state of old and new
// trace their `this.xxx` and hit bugs on trace swap.
// I think a better model is to create a new Plugin instance for each trace, and
// pass the Trace interface in the ctor. In this way they can save it in
// `this.trace` if they want, and keep all their trace-related state there.
// The number of plugins that want to do things before a trace is loaded is
// extremely low and I'd much rather treat that as a special case (e.g., by
// having a two different factories in the PluginDescriptor, one for App and
// one factory invoked on each new trace. Such a model would be incredibly more
// robust.

export interface PerfettoPlugin {
  // Lifecycle methods.
  onActivate?(ctx: App): void;
  onTraceLoad?(ctx: Trace): Promise<void>;
  onTraceReady?(ctx: Trace): Promise<void>;
  onTraceUnload?(ctx: Trace): Promise<void>;

  // Extension points.
  metricVisualisations?(ctx: App): MetricVisualisation[];
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
  new (): PerfettoPlugin;
}
// Plugins can be class refs or concrete plugin implementations.

export type PluginFactory = PluginClass | PerfettoPlugin;

export interface PluginDescriptor {
  // A unique string for your plugin. To ensure the name is unique you
  // may wish to use a URL with reversed components in the manner of
  // Java package names.
  pluginId: string;

  // The plugin factory used to instantiate the plugin object, or if this is
  // an actual plugin implementation, it's just used as-is.
  plugin: PluginFactory;
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
