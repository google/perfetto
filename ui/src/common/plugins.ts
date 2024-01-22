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

import {v4 as uuidv4} from 'uuid';

import {Disposable, Trash} from '../base/disposable';
import {assertFalse} from '../base/logging';
import {time} from '../base/time';
import {globals} from '../frontend/globals';
import {
  Command,
  EngineProxy,
  MetricVisualisation,
  Migrate,
  Plugin,
  PluginClass,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  PrimaryTrackSortKey,
  Store,
  TrackDescriptor,
  TrackPredicate,
  TrackRef,
} from '../public';
import {Engine} from '../trace_processor/engine';

import {Actions} from './actions';
import {Registry} from './registry';
import {SCROLLING_TRACK_GROUP} from './state';

// Every plugin gets its own PluginContext. This is how we keep track
// what each plugin is doing and how we can blame issues on particular
// plugins.
// The PluginContext exists for the whole duration a plugin is active.
export class PluginContextImpl implements PluginContext, Disposable {
  private trash = new Trash();
  private alive = true;

  readonly sidebar = {
    hide() {
      globals.dispatch(Actions.setSidebar({
        visible: false,
      }));
    },
    show() {
      globals.dispatch(Actions.setSidebar({
        visible: true,
      }));
    },
    isVisible() {
      return globals.state.sidebarVisible;
    },
  };

  registerCommand(cmd: Command): void {
    // Silently ignore if context is dead.
    if (!this.alive) return;

    const {id} = cmd;
    assertFalse(this.commandRegistry.has(id));
    this.commandRegistry.set(id, cmd);

    this.trash.add({
      dispose: () => {
        this.commandRegistry.delete(id);
      },
    });
  };

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  runCommand(id: string, ...args: any[]): any {
    return globals.commandManager.runCommand(id, ...args);
  };

  get commands(): Command[] {
    return globals.commandManager.commands;
  }

  constructor(
      readonly pluginId: string,
      private commandRegistry: Map<string, Command>) {}

  dispose(): void {
    this.trash.dispose();
    this.alive = false;
  }
}

// This PluginContextTrace implementation provides the plugin access to trace
// related resources, such as the engine and the store.
// The PluginContextTrace exists for the whole duration a plugin is active AND a
// trace is loaded.
class PluginContextTraceImpl implements PluginContextTrace, Disposable {
  private trash = new Trash();
  private alive = true;

  constructor(
      private ctx: PluginContext, readonly engine: EngineProxy,
      readonly trackRegistry: Map<string, TrackDescriptor>,
      private defaultTracks: Set<TrackRef>,
      private commandRegistry: Map<string, Command>) {
    this.trash.add(engine);
  }

  registerCommand(cmd: Command): void {
    // Silently ignore if context is dead.
    if (!this.alive) return;

    const {id} = cmd;
    assertFalse(this.commandRegistry.has(id));
    this.commandRegistry.set(id, cmd);

    this.trash.add({
      dispose: () => {
        this.commandRegistry.delete(id);
      },
    });
  }

  registerTrack(trackDesc: TrackDescriptor): void {
    // Silently ignore if context is dead.
    if (!this.alive) return;
    this.trackRegistry.set(trackDesc.uri, trackDesc);
    this.trash.addCallback(() => this.trackRegistry.delete(trackDesc.uri));
  }

  addDefaultTrack(track: TrackRef): void {
    this.defaultTracks.add(track);
    this.trash.addCallback(() => this.defaultTracks.delete(track));
  }

  registerStaticTrack(track: TrackDescriptor&TrackRef): void {
    this.registerTrack(track);
    this.addDefaultTrack(track);
  }

  get commands(): Command[] {
    return this.ctx.commands;
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  runCommand(id: string, ...args: any[]): any {
    return this.ctx.runCommand(id, ...args);
  }

  get sidebar() {
    return this.ctx.sidebar;
  }

  readonly tabs = {
    openQuery: (query: string, title: string) => {
      globals.openQuery(query, title);
    },
  };

  get pluginId(): string {
    return this.ctx.pluginId;
  }

  readonly timeline = {
    // Add a new track to the timeline, returning its key.
    addTrack(uri: string, displayName: string, params?: unknown): string {
      const trackKey = uuidv4();
      globals.dispatch(Actions.addTrack({
        key: trackKey,
        uri,
        name: displayName,
        params,
        trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
        trackGroup: SCROLLING_TRACK_GROUP,
      }));
      return trackKey;
    },

    removeTrack(key: string):
        void {
          globals.dispatch(Actions.removeTracks({trackKeys: [key]}));
        },

    pinTrack(key: string) {
      if (!isPinned(key)) {
        globals.dispatch(Actions.toggleTrackPinned({trackKey: key}));
      }
    },

    unpinTrack(key: string) {
      if (isPinned(key)) {
        globals.dispatch(Actions.toggleTrackPinned({trackKey: key}));
      }
    },

    pinTracksByPredicate(predicate: TrackPredicate) {
      const tracks = Object.values(globals.state.tracks);
      for (const track of tracks) {
        const tags = {
          name: track.name,
        };
        if (predicate(tags) && !isPinned(track.key)) {
          globals.dispatch(Actions.toggleTrackPinned({
            trackKey: track.key,
          }));
        }
      }
    },

    unpinTracksByPredicate(predicate: TrackPredicate) {
      const tracks = Object.values(globals.state.tracks);
      for (const track of tracks) {
        const tags = {
          name: track.name,
        };
        if (predicate(tags) && isPinned(track.key)) {
          globals.dispatch(Actions.toggleTrackPinned({
            trackKey: track.key,
          }));
        }
      }
    },

    removeTracksByPredicate(predicate: TrackPredicate) {
      const trackKeysToRemove = Object.values(globals.state.tracks)
                                    .filter((track) => {
                                      const tags = {
                                        name: track.name,
                                      };
                                      return predicate(tags);
                                    })
                                    .map((trackState) => trackState.key);

      globals.dispatch(Actions.removeTracks({trackKeys: trackKeysToRemove}));
    },

    get tracks():
        TrackRef[] {
          return Object.values(globals.state.tracks).map((trackState) => {
            return {
              displayName: trackState.name,
              uri: trackState.uri,
              params: trackState.params,
            };
          });
        },

    panToTimestamp(ts: time):
        void {
          globals.panToTimestamp(ts);
        },
  };

  dispose(): void {
    this.trash.dispose();
    this.alive = false;
  }

  mountStore<T>(migrate: Migrate<T>): Store<T> {
    return globals.store.createSubStore(['plugins', this.pluginId], migrate);
  }
}

function isPinned(trackId: string): boolean {
  return globals.state.pinnedTracks.includes(trackId);
}

// 'Static' registry of all known plugins.
export class PluginRegistry extends Registry<PluginDescriptor> {
  constructor() {
    super((info) => info.pluginId);
  }
}

interface PluginDetails {
  plugin: Plugin;
  context: PluginContext&Disposable;
  traceContext?: PluginContextTraceImpl;
}

function isPluginClass(v: unknown): v is PluginClass {
  return typeof v === 'function' && !!(v.prototype.onActivate);
}

function makePlugin(info: PluginDescriptor): Plugin {
  const {plugin} = info;

  if (typeof plugin === 'function') {
    if (isPluginClass(plugin)) {
      const PluginClass = plugin;
      return new PluginClass();
    } else {
      return plugin();
    }
  } else {
    return plugin;
  }
}

export class PluginManager {
  private registry: PluginRegistry;
  private plugins: Map<string, PluginDetails>;
  private engine?: Engine;
  readonly trackRegistry = new Map<string, TrackDescriptor>();
  readonly commandRegistry = new Map<string, Command>();
  readonly defaultTracks = new Set<TrackRef>();

  constructor(registry: PluginRegistry) {
    this.registry = registry;
    this.plugins = new Map();
  }

  activatePlugin(id: string): void {
    if (this.isActive(id)) {
      return;
    }

    const pluginInfo = this.registry.get(id);
    const plugin = makePlugin(pluginInfo);

    const context = new PluginContextImpl(id, this.commandRegistry);

    plugin.onActivate(context);

    const pluginDetails: PluginDetails = {
      plugin,
      context,
    };

    // If a trace is already loaded when plugin is activated, make sure to
    // call onTraceLoad().
    if (this.engine) {
      this.doPluginTraceLoad(pluginDetails, this.engine, id);
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

  getPluginContext(pluginId: string): PluginDetails|undefined {
    return this.plugins.get(pluginId);
  }

  findPotentialTracks(): TrackRef[] {
    return Array.from(this.defaultTracks);
  }

  async onTraceLoad(engine: Engine): Promise<void> {
    this.engine = engine;
    const plugins = Array.from(this.plugins.entries());
    const promises = plugins.map(([id, pluginDetails]) => {
      return this.doPluginTraceLoad(pluginDetails, engine, id);
    });
    await Promise.all(promises);
  }

  onTraceClose() {
    for (const pluginDetails of this.plugins.values()) {
      maybeDoPluginTraceUnload(pluginDetails);
    }
    this.engine = undefined;
  }

  commands(): Command[] {
    return Array.from(this.commandRegistry.values());
  }

  metricVisualisations(): MetricVisualisation[] {
    return Array.from(this.plugins.values()).flatMap((ctx) => {
      const tracePlugin = ctx.plugin;
      if (tracePlugin.metricVisualisations) {
        return tracePlugin.metricVisualisations(ctx.context);
      } else {
        return [];
      }
    });
  }

  // Look up track into for a given track's URI.
  // Returns |undefined| if no track can be found.
  resolveTrackInfo(uri: string): TrackDescriptor|undefined {
    return this.trackRegistry.get(uri);
  }

  private async doPluginTraceLoad(
      pluginDetails: PluginDetails, engine: Engine,
      pluginId: string): Promise<void> {
    const {plugin, context} = pluginDetails;

    const engineProxy = engine.getProxy(pluginId);

    const traceCtx = new PluginContextTraceImpl(
        context,
        engineProxy,
        this.trackRegistry,
        this.defaultTracks,
        this.commandRegistry);
    pluginDetails.traceContext = traceCtx;

    const result = plugin.onTraceLoad?.(traceCtx);
    return Promise.resolve(result);
  }
}

function maybeDoPluginTraceUnload(pluginDetails: PluginDetails): void {
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
