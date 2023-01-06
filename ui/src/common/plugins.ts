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

import {Engine} from '../common/engine';
import {
  TrackControllerFactory,
  trackControllerRegistry,
} from '../controller/track_controller';
import {TrackCreator} from '../frontend/track';
import {trackRegistry} from '../frontend/track_registry';

import {
  PluginContext,
  PluginInfo,
  TrackInfo,
  TrackProvider,
} from './plugin_api';
import {Registry} from './registry';

// Every plugin gets its own PluginContext. This is how we keep track
// what each plugin is doing and how we can blame issues on particular
// plugins.
export class PluginContextImpl implements PluginContext {
  readonly pluginId: string;
  private trackProviders: TrackProvider[];

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
  // ==================================================================

  // ==================================================================
  // Internal facing API:
  findPotentialTracks(engine: Engine): Promise<TrackInfo[]>[] {
    const proxy = engine.getProxy(this.pluginId);
    return this.trackProviders.map((f) => f(proxy));
  }

  // Unload the plugin. Ideally no plugin code runs after this point.
  // PluginContext should unregister everything.
  revoke() {
    // TODO(hjd): Remove from trackControllerRegistry, trackRegistry,
    // etc.
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
}

// TODO(hjd): Sort out the story for global singletons like these:
export const pluginRegistry = new PluginRegistry();
export const pluginManager = new PluginManager(pluginRegistry);
