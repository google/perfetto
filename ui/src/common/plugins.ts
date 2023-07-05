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


import {
  TrackControllerFactory,
  trackControllerRegistry,
} from '../controller/track_controller';
import {TrackCreator} from '../frontend/track';
import {trackRegistry} from '../frontend/track_registry';
import {
  Command,
  EngineProxy,
  PluginContext,
  PluginInfo,
  Store,
  TracePlugin,
  TracePluginFactory,
  TrackInfo,
  TrackProvider,
} from '../public';

import {Engine} from './engine';
import {Registry} from './registry';
import {State} from './state';

interface TracePluginContext {
  plugin: TracePlugin;
  store: Store<unknown>;
}

// Every plugin gets its own PluginContext. This is how we keep track
// what each plugin is doing and how we can blame issues on particular
// plugins.
export class PluginContextImpl implements PluginContext {
  readonly pluginId: string;
  private trackProviders: TrackProvider[];
  private tracePluginFactory?: TracePluginFactory<any>;
  private _tracePluginCtx?: TracePluginContext;

  constructor(pluginId: string) {
    this.pluginId = pluginId;
    this.trackProviders = [];
  }

  // ==================================================================
  // The plugin facing API of PluginContext:
  registerTrackController(track: TrackControllerFactory): void {
    trackControllerRegistry.register(track);
  }

  registerTrack(track: TrackCreator): void {
    trackRegistry.register(track);
  }

  registerTrackProvider(provider: TrackProvider) {
    this.trackProviders.push(provider);
  }

  registerTracePluginFactory<T>(pluginFactory: TracePluginFactory<T>): void {
    this.tracePluginFactory = pluginFactory;
  }
  // ==================================================================

  // ==================================================================
  // Internal facing API:
  findPotentialTracks(engine: Engine): Promise<TrackInfo[]>[] {
    const proxy = engine.getProxy(this.pluginId);
    return this.trackProviders.map((f) => f(proxy));
  }

  onTraceLoad(store: Store<State>, engine: Engine): void {
    const TracePluginClass = this.tracePluginFactory;
    if (TracePluginClass) {
      // Make an engine proxy for this plugin.
      const engineProxy: EngineProxy = engine.getProxy(this.pluginId);

      // Extract the initial state and pass to the plugin factory for migration.
      const initialState = store.state.plugins[this.pluginId];
      const migratedState = TracePluginClass.migrate(initialState);

      // Store the initial state in our root store.
      store.edit((draft) => {
        draft.plugins[this.pluginId] = migratedState;
      });

      // Create a proxy store for our plugin to use.
      const storeProxy = store.createProxy<unknown>(['plugins', this.pluginId]);

      // Instantiate the plugin.
      this._tracePluginCtx = {
        plugin: new TracePluginClass(storeProxy, engineProxy),
        store: storeProxy,
      };
    }
  }

  onTraceClosed() {
    if (this._tracePluginCtx) {
      this._tracePluginCtx.plugin.dispose();
      this._tracePluginCtx.store.dispose();
      this._tracePluginCtx = undefined;
    }
  }

  get tracePlugin(): TracePlugin|undefined {
    return this._tracePluginCtx?.plugin;
  }

  // Unload the plugin. Ideally no plugin code runs after this point.
  // PluginContext should unregister everything.
  revoke() {
    // TODO(hjd): Remove from trackControllerRegistry, trackRegistry,
    // etc.
    // TODO(stevegolton): Dispose the trace plugin.
  }
  // ==================================================================
}

// 'Static' registry of all known plugins.
export class PluginRegistry extends Registry<PluginInfo> {
  constructor() {
    super((info) => info.pluginId);
  }
}

export class PluginManager {
  private registry: PluginRegistry;
  private contexts: Map<string, PluginContextImpl>;

  constructor(registry: PluginRegistry) {
    this.registry = registry;
    this.contexts = new Map();
  }

  activatePlugin(pluginId: string): void {
    if (this.isActive(pluginId)) {
      return;
    }
    const pluginInfo = this.registry.get(pluginId);
    const context = new PluginContextImpl(pluginId);
    this.contexts.set(pluginId, context);
    pluginInfo.activate(context);
  }

  deactivatePlugin(pluginId: string): void {
    const context = this.getPluginContext(pluginId);
    if (context === undefined) {
      return;
    }
    context.revoke();
    this.contexts.delete(pluginId);
  }

  isActive(pluginId: string): boolean {
    return this.getPluginContext(pluginId) !== undefined;
  }

  getPluginContext(pluginId: string): PluginContextImpl|undefined {
    return this.contexts.get(pluginId);
  }

  findPotentialTracks(engine: Engine): Promise<TrackInfo[]>[] {
    const promises = [];
    for (const context of this.contexts.values()) {
      for (const promise of context.findPotentialTracks(engine)) {
        promises.push(promise);
      }
    }
    return promises;
  }

  onTraceLoad(store: Store<State>, engine: Engine): void {
    for (const context of this.contexts.values()) {
      context.onTraceLoad(store, engine);
    }
  }

  onTraceClose() {
    for (const context of this.contexts.values()) {
      context.onTraceClosed();
    }
  }

  commands(): Command[] {
    return Array.from(this.contexts.values()).flatMap((ctx) => {
      const tracePlugin = ctx.tracePlugin;
      if (tracePlugin && tracePlugin.commands) {
        return tracePlugin.commands();
      } else {
        return [];
      }
    });
  }
}

// TODO(hjd): Sort out the story for global singletons like these:
export const pluginRegistry = new PluginRegistry();
export const pluginManager = new PluginManager(pluginRegistry);
