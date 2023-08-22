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
// The PluginContext exists for the whole duration a plugin is active.
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

// This TracePluginContext implementation provides the plugin access to trace
// related resources, such as the engine and the store.
// The TracePluginContext exists for the whole duration a plugin is active AND a
// trace is loaded.
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

    const pluginInfo = this.registry.get(id);
    const plugin = makePlugin(pluginInfo);

    const viewerProxy = viewer.getProxy(id);
    const context = new PluginContextImpl(id, viewerProxy);

    plugin.onActivate && plugin.onActivate(context);

    const pluginDetails: PluginDetails<unknown> = {
      plugin,
      context,
    };

    // If a trace is already loaded when plugin is activated, make sure to
    // call onTraceLoad().
    if (this.engine) {
      doPluginTraceLoad(pluginDetails, this.engine, id);
    }

    this.plugins.set(id, pluginDetails);
  }

  deactivatePlugin(id: string): void {
    const pluginDetails = this.getPluginContext(id);
    if (pluginDetails === undefined) {
      return;
    }
    const {context, plugin} = pluginDetails;

    maybeDoPluginTraceUnload(pluginDetails);

    plugin.onDeactivate && plugin.onDeactivate(context);
    context.dispose();

    this.plugins.delete(id);
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
      doPluginTraceLoad(pluginDetails, engine, id);
    }
  }

  onTraceClose() {
    for (const pluginDetails of this.plugins.values()) {
      maybeDoPluginTraceUnload(pluginDetails);
    }
    this.engine = undefined;
  }

  commands(): Command[] {
    return Array.from(this.plugins.values()).flatMap((ctx) => {
      const plugin = ctx.plugin;
      let commands: Command[] = [];

      if (plugin.commands) {
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

function doPluginTraceLoad(
    pluginDetails: PluginDetails<unknown>, engine: Engine, pluginId: string):
    void {
  const {plugin, context} = pluginDetails;

  // Migrate state & write back to store.
  if (plugin.migrate) {
    const initialState = globals.store.state.plugins[pluginId];
    const migratedState = plugin.migrate(initialState);
    globals.store.edit((draft) => {
      draft.plugins[pluginId] = migratedState;
    });
  }

  // Create proxies & trace context.
  const proxyStore = globals.store.createProxy<unknown>(['plugins', pluginId]);
  const engineProxy = engine.getProxy(pluginId);
  const traceCtx = new TracePluginContextImpl(context, proxyStore, engineProxy);
  pluginDetails.traceContext = traceCtx;

  // TODO(stevegolton): Await onTraceLoad.
  plugin.onTraceLoad && plugin.onTraceLoad(traceCtx);
}

function maybeDoPluginTraceUnload(pluginDetails: PluginDetails<unknown>): void {
  const {traceContext, plugin} = pluginDetails;

  if (traceContext) {
    // TODO(stevegolton): Await onTraceUnload.
    plugin.onTraceUnload && plugin.onTraceUnload(traceContext);
    traceContext.dispose();
    pluginDetails.traceContext = undefined;
  }
}


// TODO(hjd): Sort out the story for global singletons like these:
export const pluginRegistry = new PluginRegistry();
export const pluginManager = new PluginManager(pluginRegistry);
