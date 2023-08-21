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

import {Disposable, Trash} from '../base/disposable';
import {ViewerImpl, ViewerProxy} from '../common/viewer';
import {
  TrackControllerFactory,
  trackControllerRegistry,
} from '../controller/track_controller';
import {globals} from '../frontend/globals';
import {TrackCreator} from '../frontend/track';
import {trackRegistry} from '../frontend/track_registry';
import {
  Command,
  EngineProxy,
  MetricVisualisation,
  Plugin,
  PluginClass,
  PluginContext,
  PluginInfo,
  Store,
  TracePluginContext,
  TrackInfo,
  Viewer,
} from '../public';

import {Engine} from './engine';
import {Registry} from './registry';

// Every plugin gets its own PluginContext. This is how we keep track
// what each plugin is doing and how we can blame issues on particular
// plugins.
export class PluginContextImpl implements PluginContext, Disposable {
  readonly pluginId: string;
  readonly viewer: ViewerProxy;
  private trash = new Trash();

  constructor(pluginId: string, viewer: ViewerProxy) {
    this.pluginId = pluginId;

    this.viewer = viewer;
    this.trash.add(viewer);
  }

  registerTrackController(track: TrackControllerFactory): void {
    const unregister = trackControllerRegistry.register(track);
    this.trash.add(unregister);
  }

  registerTrack(track: TrackCreator): void {
    const unregister = trackRegistry.register(track);
    this.trash.add(unregister);
  }

  dispose(): void {
    this.trash.dispose();
  }
}

// Implementation the trace plugin context with trace-relevant properties.
class TracePluginContextImpl<T> implements TracePluginContext<T>, Disposable {
  private ctx: PluginContext;
  readonly engine: EngineProxy;
  readonly store: Store<T>;
  private trash = new Trash();

  constructor(ctx: PluginContext, store: Store<T>, engine: EngineProxy) {
    this.ctx = ctx;

    this.engine = engine;
    this.trash.add(engine);

    this.store = store;
    this.trash.add(store);
  }

  registerTrackController(track: TrackControllerFactory): void {
    this.ctx.registerTrackController(track);
  }

  registerTrack(track: TrackCreator): void {
    this.ctx.registerTrack(track);
  }

  get viewer(): Viewer {
    return this.ctx.viewer;
  }

  dispose(): void {
    this.trash.dispose();
  }
}

// 'Static' registry of all known plugins.
export class PluginRegistry extends Registry<PluginInfo<unknown>> {
  constructor() {
    super((info) => info.pluginId);
  }
}

interface PluginDetails<T> {
  plugin: Plugin<T>;
  context: PluginContextImpl;
  traceContext?: TracePluginContextImpl<T>;
}

function isPluginClass<T>(v: unknown): v is PluginClass<T> {
  return typeof v === 'function' && !!(v.prototype.onActivate);
}

function makePlugin<T>(info: PluginInfo<T>): Plugin<T> {
  const {plugin: pluginFactory} = info;

  if (typeof pluginFactory === 'function') {
    if (isPluginClass(pluginFactory)) {
      const PluginClass = pluginFactory;
      return new PluginClass();
    } else {
      return pluginFactory();
    }
  } else {
    // pluginFactory is the plugin!
    const plugin = pluginFactory;
    return plugin;
  }
}

export class PluginManager {
  private registry: PluginRegistry;
  private plugins: Map<string, PluginDetails<unknown>>;
  private engine?: Engine;

  constructor(registry: PluginRegistry) {
    this.registry = registry;
    this.plugins = new Map();
  }

  activatePlugin(id: string, viewer: ViewerImpl): void {
    if (this.isActive(id)) {
      return;
    }

    // This is where the plugin context is created, and where we call
    // onInit() on the plugin.
    const pluginInfo = this.registry.get(id);
    const plugin = makePlugin(pluginInfo);
    const viewerProxy = viewer.getProxy(id);

    // Create a proxy store for our plugin to use.
    const context = new PluginContextImpl(id, viewerProxy);
    plugin.onActivate && plugin.onActivate(context);

    const pluginDetails: PluginDetails<unknown> = {
      plugin,
      context,
    };

    // If we already have a trace when the plugin is activated, call
    // onTraceLoad() on the plugin and store the traceContext.
    if (this.engine) {
      this.initTracePlugin(pluginDetails, this.engine, id);
    }

    this.plugins.set(id, pluginDetails);
  }

  deactivatePlugin(pluginId: string): void {
    const pluginDetails = this.getPluginContext(pluginId);
    if (pluginDetails === undefined) {
      return;
    }
    const {context, plugin, traceContext} = pluginDetails;

    if (traceContext) {
      plugin.onTraceUnload && plugin.onTraceUnload(traceContext);
    }

    plugin.onDeactivate && plugin.onDeactivate(context);
    context.dispose();

    this.plugins.delete(pluginId);
  }

  isActive(pluginId: string): boolean {
    return this.getPluginContext(pluginId) !== undefined;
  }

  getPluginContext(pluginId: string): PluginDetails<unknown>|undefined {
    return this.plugins.get(pluginId);
  }

  findPotentialTracks(): Promise<TrackInfo[]>[] {
    const promises: Promise<TrackInfo[]>[] = [];
    for (const {plugin, traceContext} of this.plugins.values()) {
      if (plugin.findPotentialTracks && traceContext) {
        const promise = plugin.findPotentialTracks(traceContext);
        promises.push(promise);
      }
    }
    return promises;
  }

  onTraceLoad(engine: Engine): void {
    this.engine = engine;
    for (const [id, pluginDetails] of this.plugins) {
      this.initTracePlugin(pluginDetails, engine, id);
    }
  }

  private initTracePlugin(
      pluginDetails: PluginDetails<unknown>, engine: Engine, id: string): void {
    const {plugin, context} = pluginDetails;

    const engineProxy = engine.getProxy(id);
    if (plugin.migrate) {
      // Extract the initial state and migrate.
      const initialState = globals.store.state.plugins[id];
      const migratedState = plugin.migrate(initialState);

      // Write the the migrated state back to our root store.
      globals.store.edit((draft) => {
        draft.plugins[id] = migratedState;
      });
    }

    const proxyStore = globals.store.createProxy<unknown>(['plugins', id]);
    const traceCtx =
        new TracePluginContextImpl(context, proxyStore, engineProxy);

    // TODO(stevegolton): We should probably wait for this to complete.
    plugin.onTraceLoad && plugin.onTraceLoad(traceCtx);
    pluginDetails.traceContext = traceCtx;
  }

  onTraceClose() {
    for (const pluginDetails of this.plugins.values()) {
      const {traceContext, plugin} = pluginDetails;

      if (traceContext) {
        if (plugin.onTraceUnload) {
          plugin.onTraceUnload(traceContext);
        }
        traceContext.dispose();
      }

      pluginDetails.traceContext = undefined;
    }
    this.engine = undefined;
  }

  commands(): Command[] {
    return Array.from(this.plugins.values()).flatMap((ctx) => {
      const plugin = ctx.plugin;
      let commands: Command[] = [];

      if (plugin && plugin.commands) {
        commands = commands.concat(plugin.commands(ctx.context));
      }

      if (ctx.traceContext && plugin.traceCommands) {
        commands = commands.concat(plugin.traceCommands(ctx.traceContext));
      }

      return commands;
    });
  }

  metricVisualisations(): MetricVisualisation[] {
    return Array.from(this.plugins.values()).flatMap((ctx) => {
      const tracePlugin = ctx.plugin;
      if (tracePlugin && tracePlugin.metricVisualisations) {
        return tracePlugin.metricVisualisations(ctx.context);
      } else {
        return [];
      }
    });
  }
}

// TODO(hjd): Sort out the story for global singletons like these:
export const pluginRegistry = new PluginRegistry();
export const pluginManager = new PluginManager(pluginRegistry);
