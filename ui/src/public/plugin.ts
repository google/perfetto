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

/**
 * This interface defines the shape of the plugins's class constructor (i.e. the
 * the constructor and all static members of the plugin's class.
 *
 * This class constructor is registered with the core.
 *
 * On trace load, the core will create a new class instance by calling new on
 * this constructor and then call its onTraceLoad() function.
 */
export interface PerfettoPluginStatic<T extends PerfettoPlugin> {
  readonly id: string;
  readonly dependencies?: ReadonlyArray<PerfettoPluginStatic<PerfettoPlugin>>;
  onActivate?(app: App): void;
  metricVisualisations?(): MetricVisualisation[];
  new (trace: Trace): T;
}

/**
 * This interface defines the shape of a plugin's trace-scoped instance, which
 * is created from the class constructor above at trace load time.
 */
export interface PerfettoPlugin {
  onTraceLoad?(ctx: Trace): Promise<void>;
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

export interface PluginManager {
  getPlugin<T extends PerfettoPlugin>(plugin: PerfettoPluginStatic<T>): T;
  metricVisualisations(): MetricVisualisation[];
}
