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
import {time} from '../base/time';
import {globals} from '../frontend/globals';
import {
  Command,
  DetailsPanel,
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
  TabDescriptor,
  TrackDescriptor,
  TrackPredicate,
  GroupPredicate,
  TrackRef,
} from '../public';
import {Engine} from '../trace_processor/engine';

import {Actions} from './actions';
import {Registry} from './registry';
import {SCROLLING_TRACK_GROUP} from './state';
import {addQueryResultsTab} from '../frontend/query_result_tab';
import {Flag, featureFlags} from '../core/feature_flags';
import {assertExists} from '../base/logging';
import {raf} from '../core/raf_scheduler';
import {defaultPlugins} from './default_plugins';

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

    const disposable = globals.commandManager.registry.register(cmd);
    this.trash.add(disposable);
  };

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  runCommand(id: string, ...args: any[]): any {
    return globals.commandManager.runCommand(id, ...args);
  };

  constructor(readonly pluginId: string) {}

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

  constructor(private ctx: PluginContext, readonly engine: EngineProxy) {
    this.trash.add(engine);
  }

  registerCommand(cmd: Command): void {
    // Silently ignore if context is dead.
    if (!this.alive) return;

    const disposable = globals.commandManager.registry.register(cmd);
    this.trash.add(disposable);
  }

  registerTrack(trackDesc: TrackDescriptor): void {
    // Silently ignore if context is dead.
    if (!this.alive) return;
    globals.trackManager.registerTrack(trackDesc);
    this.trash.addCallback(
      () => globals.trackManager.unregisterTrack(trackDesc.uri));
  }

  addDefaultTrack(track: TrackRef): void {
    globals.trackManager.addDefaultTrack(track);
    this.trash.addCallback(
      () => globals.trackManager.removeDefaultTrack(track));
  }

  registerStaticTrack(track: TrackDescriptor&TrackRef): void {
    this.registerTrack(track);
    this.addDefaultTrack(track);
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  runCommand(id: string, ...args: any[]): any {
    return this.ctx.runCommand(id, ...args);
  }

  registerTab(desc: TabDescriptor): void {
    if (!this.alive) return;

    const unregister = globals.tabManager.registerTab(desc);
    this.trash.add(unregister);
  }

  addDefaultTab(uri: string): void {
    const remove = globals.tabManager.addDefaultTab(uri);
    this.trash.add(remove);
  }

  registerDetailsPanel(section: DetailsPanel): void {
    if (!this.alive) return;

    const tabMan = globals.tabManager;
    const unregister = tabMan.registerDetailsPanel(section);
    this.trash.add(unregister);
  }

  get sidebar() {
    return this.ctx.sidebar;
  }

  readonly tabs = {
    openQuery: (query: string, title: string) => {
      addQueryResultsTab({query, title});
    },

    showTab(uri: string):
        void {
      globals.dispatch(Actions.showTab({uri}));
    },

    hideTab(uri: string):
        void {
      globals.dispatch(Actions.hideTab({uri}));
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

    expandGroupsByPredicate(predicate: GroupPredicate) {
      const groups = globals.state.trackGroups;
      const groupsToExpand = Object.values(groups)
        .filter((group) => group.collapsed)
        .filter((group) => {
          const ref = {
            displayName: group.name,
            collapsed: group.collapsed,
          };
          return predicate(ref);
        })
        .map((group) => group.id);

      for (const trackGroupId of groupsToExpand) {
        globals.dispatch(Actions.toggleTrackGroupCollapsed({trackGroupId}));
      }
    },

    collapseGroupsByPredicate(predicate: GroupPredicate) {
      const groups = globals.state.trackGroups;
      const groupsToCollapse = Object.values(groups)
        .filter((group) => !group.collapsed)
        .filter((group) => {
          const ref = {
            displayName: group.name,
            collapsed: group.collapsed,
          };
          return predicate(ref);
        })
        .map((group) => group.id);

      for (const trackGroupId of groupsToCollapse) {
        globals.dispatch(Actions.toggleTrackGroupCollapsed({trackGroupId}));
      }
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

export interface PluginDetails {
  plugin: Plugin;
  context: PluginContext&Disposable;
  traceContext?: PluginContextTraceImpl;
  previousOnTraceLoadTimeMillis?: number;
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
  private _plugins: Map<string, PluginDetails>;
  private engine?: Engine;
  private flags = new Map<string, Flag>();

  constructor(registry: PluginRegistry) {
    this.registry = registry;
    this._plugins = new Map();
  }

  get plugins(): Map<string, PluginDetails> {
    return this._plugins;
  }

  // Must only be called once on startup
  async initialize(): Promise<void> {
    for (const plugin of pluginRegistry.values()) {
      const id = `plugin_${plugin.pluginId}`;
      const name = `Plugin: ${plugin.pluginId}`;
      const flag = featureFlags.register({
        id,
        name,
        description: `Overrides '${id}' plugin.`,
        defaultValue: defaultPlugins.includes(plugin.pluginId),
      });
      this.flags.set(plugin.pluginId, flag);
      if (flag.get()) {
        await this.activatePlugin(plugin.pluginId);
      }
    }
  }

  /**
   * Enable plugin flag - i.e. configure a plugin to start on boot.
   * @param id The ID of the plugin.
   * @param now Optional: If true, also activate the plugin now.
   */
  async enablePlugin(id: string, now?: boolean): Promise<void> {
    const flag = this.flags.get(id);
    if (flag) {
      flag.set(true);
    }
    now && await this.activatePlugin(id);
  }

  /**
   * Disable plugin flag - i.e. configure a plugin not to start on boot.
   * @param id The ID of the plugin.
   * @param now Optional: If true, also deactivate the plugin now.
   */
  async disablePlugin(id: string, now?: boolean): Promise<void> {
    const flag = this.flags.get(id);
    if (flag) {
      flag.set(false);
    }
    now && await this.deactivatePlugin(id);
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

    const context = new PluginContextImpl(id);

    plugin.onActivate(context);

    const pluginDetails: PluginDetails = {
      plugin,
      context,
    };

    // If a trace is already loaded when plugin is activated, make sure to
    // call onTraceLoad().
    if (this.engine) {
      await doPluginTraceLoad(pluginDetails, this.engine, id);
    }

    this._plugins.set(id, pluginDetails);

    raf.scheduleFullRedraw();
  }

  /**
   * Stop a plugin just for this session. This setting is not persisted.
   * @param id The ID of the plugin to stop.
   */
  async deactivatePlugin(id: string): Promise<void> {
    const pluginDetails = this.getPluginContext(id);
    if (pluginDetails === undefined) {
      return;
    }
    const {context, plugin} = pluginDetails;

    await doPluginTraceUnload(pluginDetails);

    plugin.onDeactivate && plugin.onDeactivate(context);
    context.dispose();

    this._plugins.delete(id);

    raf.scheduleFullRedraw();
  }

  /**
   * Restore all plugins enable/disabled flags to their default values.
   * @param now Optional: Also activates/deactivates plugins to match flag
   * settings.
   */
  async restoreDefaults(now?: boolean): Promise<void> {
    for (const plugin of pluginRegistry.values()) {
      const pluginId = plugin.pluginId;
      const flag = assertExists(this.flags.get(pluginId));
      flag.reset();
      if (now) {
        if (flag.get()) {
          await this.activatePlugin(plugin.pluginId);
        } else {
          await this.deactivatePlugin(plugin.pluginId);
        }
      }
    }
  }

  isActive(pluginId: string): boolean {
    return this.getPluginContext(pluginId) !== undefined;
  }

  isEnabled(pluginId: string): boolean {
    return Boolean(this.flags.get(pluginId)?.get());
  }

  getPluginContext(pluginId: string): PluginDetails|undefined {
    return this._plugins.get(pluginId);
  }

  async onTraceLoad(engine: Engine): Promise<void> {
    this.engine = engine;
    const plugins = Array.from(this._plugins.entries());
    // Awaiting all plugins in parallel will skew timing data as later plugins
    // will spend most of their time waiting for earlier plugins to load.
    // Running in parallel will have very little performance benefit assuming
    // most plugins use the same engine, which can only process one query at a
    // time.
    for (const [id, pluginDetails] of plugins) {
      await doPluginTraceLoad(pluginDetails, engine, id);
    }
  }

  onTraceClose() {
    for (const pluginDetails of this._plugins.values()) {
      doPluginTraceUnload(pluginDetails);
    }
    this.engine = undefined;
  }

  metricVisualisations(): MetricVisualisation[] {
    return Array.from(this._plugins.values()).flatMap((ctx) => {
      const tracePlugin = ctx.plugin;
      if (tracePlugin.metricVisualisations) {
        return tracePlugin.metricVisualisations(ctx.context);
      } else {
        return [];
      }
    });
  }
}

async function doPluginTraceLoad(
  pluginDetails: PluginDetails,
  engine: Engine,
  pluginId: string): Promise<void> {
  const {plugin, context} = pluginDetails;

  const engineProxy = engine.getProxy(pluginId);

  const traceCtx = new PluginContextTraceImpl(context, engineProxy);
  pluginDetails.traceContext = traceCtx;

  const startTime = performance.now();
  const result = await Promise.resolve(plugin.onTraceLoad?.(traceCtx));
  const loadTime = performance.now() - startTime;
  pluginDetails.previousOnTraceLoadTimeMillis = loadTime;

  raf.scheduleFullRedraw();

  return result;
}

async function doPluginTraceUnload(
  pluginDetails: PluginDetails): Promise<void> {
  const {traceContext, plugin} = pluginDetails;

  if (traceContext) {
    plugin.onTraceUnload && await plugin.onTraceUnload(traceContext);
    traceContext.dispose();
    pluginDetails.traceContext = undefined;
  }
}


// TODO(hjd): Sort out the story for global singletons like these:
export const pluginRegistry = new PluginRegistry();
export const pluginManager = new PluginManager(pluginRegistry);
