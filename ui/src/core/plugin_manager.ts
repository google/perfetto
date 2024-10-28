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

import {Registry} from '../base/registry';
import {App} from '../public/app';
import {
  MetricVisualisation,
  PerfettoPlugin,
  PluginDescriptor,
} from '../public/plugin';
import {Trace} from '../public/trace';
import {defaultPlugins} from './default_plugins';
import {featureFlags, Flag} from './feature_flags';
import {TraceImpl} from './trace_impl';

// The pseudo plugin id used for the core instance of AppImpl.
export const CORE_PLUGIN_ID = '__core__';

function makePlugin(info: PluginDescriptor): PerfettoPlugin {
  const {plugin} = info;

  // Class refs are functions, concrete plugins are not
  if (typeof plugin === 'function') {
    const PluginClass = plugin;
    return new PluginClass();
  } else {
    return plugin;
  }
}

// This interface injects AppImpl's methods into PluginManager to avoid
// circular dependencies between PluginManager and AppImpl.
export interface PluginAppInterface {
  forkForPlugin(pluginId: string): App;
  get trace(): TraceImpl | undefined;
}

// Contains all the information about a plugin.
export interface PluginWrapper {
  // A reference to the plugin descriptor
  readonly desc: PluginDescriptor;

  // The feature flag used to allow users to change whether this plugin should
  // be enabled or not.
  readonly enableFlag: Flag;

  // If a plugin has been activated, the relevant context is stored here.
  activatedContext?: ActivePluginContext;
}

// Contains an active plugin's contextual information, only created at plugin
// activation time.
interface ActivePluginContext {
  // The plugin instance, which is only created at plugin activation time.
  readonly pluginInstance: PerfettoPlugin;

  // The app interface for this plugin.
  readonly app: App;

  // If a plugin has had its trace loaded, the relevant context is stored here.
  traceContext?: TracePluginContext;
}

// Contains the contextual information required by a plugin which has had a
// trace loaded.
interface TracePluginContext {
  // The trace interface for this plugin.
  readonly trace: Trace;

  // The time taken in milliseconds to execute this onTraceLoad() function.
  readonly loadTimeMs: number;
}

export class PluginManager {
  private readonly registry = new Registry<PluginWrapper>(
    (x) => x.desc.pluginId,
  );

  constructor(private readonly app: PluginAppInterface) {}

  registerPlugin(desc: PluginDescriptor) {
    const flagId = `plugin_${desc.pluginId}`;
    const name = `Plugin: ${desc.pluginId}`;
    const flag = featureFlags.register({
      id: flagId,
      name,
      description: `Overrides '${desc.pluginId}' plugin.`,
      defaultValue: defaultPlugins.includes(desc.pluginId),
    });
    this.registry.register({
      desc,
      enableFlag: flag,
    });
  }

  /**
   * Activates all registered plugins that have not already been registered.
   *
   * @param enableOverrides - The list of plugins that are enabled regardless of
   * the current flag setting.
   */
  activatePlugins(enableOverrides: ReadonlyArray<string> = []) {
    this.registry
      .valuesAsArray()
      .filter(
        (p) => p.enableFlag.get() || enableOverrides.includes(p.desc.pluginId),
      )
      .forEach((p) => {
        if (p.activatedContext) return;
        const pluginInstance = makePlugin(p.desc);
        const app = this.app.forkForPlugin(p.desc.pluginId);
        pluginInstance.onActivate?.(app);
        p.activatedContext = {
          pluginInstance,
          app,
        };
      });
  }

  async onTraceLoad(
    traceCore: TraceImpl,
    beforeEach?: (id: string) => void,
  ): Promise<void> {
    // Awaiting all plugins in parallel will skew timing data as later plugins
    // will spend most of their time waiting for earlier plugins to load.
    // Running in parallel will have very little performance benefit assuming
    // most plugins use the same engine, which can only process one query at a
    // time.
    for (const p of this.registry.values()) {
      const activePlugin = p.activatedContext;
      if (activePlugin) {
        beforeEach?.(p.desc.pluginId);
        const trace = traceCore.forkForPlugin(p.desc.pluginId);
        const before = performance.now();
        await activePlugin.pluginInstance.onTraceLoad?.(trace);
        const loadTimeMs = performance.now() - before;
        activePlugin.traceContext = {
          trace,
          loadTimeMs: loadTimeMs,
        };
        traceCore.trash.defer(() => {
          activePlugin.traceContext = undefined;
        });
      }
    }
  }

  metricVisualisations(): MetricVisualisation[] {
    return this.registry.valuesAsArray().flatMap((plugin) => {
      const activePlugin = plugin.activatedContext;
      if (activePlugin) {
        return (
          activePlugin.pluginInstance.metricVisualisations?.(
            activePlugin.app,
          ) ?? []
        );
      } else {
        return [];
      }
    });
  }

  getAllPlugins() {
    return this.registry.valuesAsArray();
  }

  getPluginContainer(id: string): PluginWrapper | undefined {
    return this.registry.tryGet(id);
  }

  getPlugin(id: string): PerfettoPlugin | undefined {
    return this.registry.tryGet(id)?.activatedContext?.pluginInstance;
  }
}
