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
import {Trace} from '../public/trace';
import {App} from '../public/app';
import {MetricVisualisation} from '../public/plugin';
import {PerfettoPlugin, PluginDescriptor} from '../public/plugin';
import {Flag, featureFlags} from '../core/feature_flags';
import {assertExists, assertTrue} from '../base/logging';
import {raf} from '../core/raf_scheduler';
import {defaultPlugins} from '../core/default_plugins';
import {TraceImpl} from '../core/trace_impl';
import {AppImpl, CORE_PLUGIN_ID} from '../core/app_impl';

// 'Static' registry of all known plugins.
export class PluginRegistry extends Registry<PluginDescriptor> {
  constructor() {
    super((info) => info.pluginId);
  }
}

export interface PluginDetails {
  plugin: PerfettoPlugin;
  app: App;
  trace?: Trace;
  previousOnTraceLoadTimeMillis?: number;
}

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

export class PluginManager {
  private registry: PluginRegistry;
  private _plugins: Map<string, PluginDetails>;
  private flags = new Map<string, Flag>();
  private _needsRestart = false;

  constructor(registry: PluginRegistry) {
    this.registry = registry;
    this._plugins = new Map();
  }

  get plugins(): Map<string, PluginDetails> {
    return this._plugins;
  }

  // Must only be called once on startup
  async initialize(): Promise<void> {
    for (const {pluginId} of pluginRegistry.values()) {
      const flagId = `plugin_${pluginId}`;
      const name = `Plugin: ${pluginId}`;
      const flag = featureFlags.register({
        id: flagId,
        name,
        description: `Overrides '${pluginId}' plugin.`,
        defaultValue: defaultPlugins.includes(pluginId),
      });
      this.flags.set(pluginId, flag);
      if (flag.get()) {
        await this.activatePlugin(pluginId);
      }
    }
  }

  /**
   * Enable plugin flag - i.e. configure a plugin to start on boot.
   * @param id The ID of the plugin.
   */
  async enablePlugin(id: string): Promise<void> {
    const flag = this.flags.get(id);
    if (flag) {
      flag.set(true);
    }
    await this.activatePlugin(id);
  }

  /**
   * Disable plugin flag - i.e. configure a plugin not to start on boot.
   * @param id The ID of the plugin.
   */
  async disablePlugin(id: string): Promise<void> {
    const flag = this.flags.get(id);
    if (flag) {
      flag.set(false);
    }
    this._needsRestart = true;
  }

  /**
   * Start a plugin just for this session. This setting is not persisted.
   * @param id The ID of the plugin to start.
   */
  async activatePlugin(id: string): Promise<void> {
    if (this.isActive(id)) {
      return;
    }

    const pluginInfo = this.registry.get(id);
    const plugin = makePlugin(pluginInfo);

    const app = AppImpl.instance.forkForPlugin(id);

    plugin.onActivate?.(app);

    const pluginDetails: PluginDetails = {plugin, app};

    // If a trace is already loaded when plugin is activated, make sure to
    // call onTraceLoad().
    const maybeTrace = AppImpl.instance.trace;
    if (maybeTrace !== undefined) {
      await doPluginTraceLoad(pluginDetails, maybeTrace);
      await doPluginTraceReady(pluginDetails);
    }

    this._plugins.set(id, pluginDetails);

    raf.scheduleFullRedraw();
  }

  /**
   * Restore all plugins enable/disabled flags to their default values.
   * Also activates new plugins to match flag settings.
   */
  async restoreDefaults(): Promise<void> {
    for (const plugin of pluginRegistry.values()) {
      const pluginId = plugin.pluginId;
      const flag = assertExists(this.flags.get(pluginId));
      flag.reset();
      if (flag.get()) {
        await this.activatePlugin(plugin.pluginId);
      } else {
        this._needsRestart = true;
      }
    }
  }

  hasPlugin(pluginId: string): boolean {
    return pluginRegistry.has(pluginId);
  }

  isActive(pluginId: string): boolean {
    return this.getPluginContext(pluginId) !== undefined;
  }

  isEnabled(pluginId: string): boolean {
    return Boolean(this.flags.get(pluginId)?.get());
  }

  getPluginContext(pluginId: string): PluginDetails | undefined {
    return this._plugins.get(pluginId);
  }

  // NOTE: here we take as argument the TraceImpl for the core. This is because
  // we pass it to doPluginTraceLoad() which uses to call forkForPlugin(id) and
  // derive a per-plugin instance.
  async onTraceLoad(
    traceCore: TraceImpl,
    beforeEach?: (id: string) => void,
  ): Promise<void> {
    assertTrue(traceCore.pluginId === CORE_PLUGIN_ID);

    // Awaiting all plugins in parallel will skew timing data as later plugins
    // will spend most of their time waiting for earlier plugins to load.
    // Running in parallel will have very little performance benefit assuming
    // most plugins use the same engine, which can only process one query at a
    // time.
    for (const [id, plugin] of this._plugins.entries()) {
      beforeEach?.(id);
      await doPluginTraceLoad(plugin, traceCore);
    }
  }

  async onTraceReady(): Promise<void> {
    const pluginsShuffled = Array.from(this._plugins.values())
      .map((plugin) => ({plugin, sort: Math.random()}))
      .sort((a, b) => a.sort - b.sort);

    for (const {plugin} of pluginsShuffled) {
      await doPluginTraceReady(plugin);
    }
  }

  onTraceClose() {
    for (const pluginDetails of this._plugins.values()) {
      doPluginTraceUnload(pluginDetails);
    }
  }

  metricVisualisations(): MetricVisualisation[] {
    return Array.from(this._plugins.values()).flatMap((ctx) => {
      const tracePlugin = ctx.plugin;
      if (tracePlugin.metricVisualisations) {
        return tracePlugin.metricVisualisations(ctx.app);
      } else {
        return [];
      }
    });
  }

  get needsRestart() {
    return this._needsRestart;
  }
}

async function doPluginTraceReady(pluginDetails: PluginDetails): Promise<void> {
  const {plugin, trace: traceContext} = pluginDetails;
  await Promise.resolve(plugin.onTraceReady?.(assertExists(traceContext)));
  raf.scheduleFullRedraw();
}

async function doPluginTraceLoad(
  pluginDetails: PluginDetails,
  traceCore: TraceImpl,
): Promise<void> {
  assertTrue(traceCore.pluginId === CORE_PLUGIN_ID);
  assertTrue(pluginDetails.app.pluginId !== CORE_PLUGIN_ID);
  const {plugin} = pluginDetails;
  const trace = traceCore.forkForPlugin(pluginDetails.app.pluginId);

  pluginDetails.trace = trace;

  const startTime = performance.now();
  await Promise.resolve(plugin.onTraceLoad?.(trace));
  const loadTime = performance.now() - startTime;
  pluginDetails.previousOnTraceLoadTimeMillis = loadTime;

  raf.scheduleFullRedraw();
}

async function doPluginTraceUnload(
  pluginDetails: PluginDetails,
): Promise<void> {
  const {trace, plugin} = pluginDetails;

  if (trace) {
    plugin.onTraceUnload && (await plugin.onTraceUnload(trace));
    pluginDetails.trace = undefined;
    // All the disposable resources created by plugins are appeneded to the
    // per-trace (not per-plugin) trash. There is no need of per-plugin dispose
    // call.
  }
}

// TODO(hjd): Sort out the story for global singletons like these:
export const pluginRegistry = new PluginRegistry();
export const pluginManager = new PluginManager(pluginRegistry);
