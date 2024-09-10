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
import {TimeSpan, time} from '../base/time';
import {globals} from '../frontend/globals';
import {TrackDescriptor} from '../public/track';
import {Trace} from '../public/trace';
import {App} from '../public/app';
import {SidebarMenuItem} from '../public/sidebar';
import {TabDescriptor} from '../public/tab';
import {MetricVisualisation} from '../public/plugin';
import {PerfettoPlugin, PluginDescriptor} from '../public/plugin';
import {Command} from '../public/command';
import {EngineBase, Engine} from '../trace_processor/engine';
import {addQueryResultsTab} from '../frontend/query_result_tab';
import {Flag, featureFlags} from '../core/feature_flags';
import {assertExists} from '../base/logging';
import {raf} from '../core/raf_scheduler';
import {defaultPlugins} from '../core/default_plugins';
import {PromptOption} from '../public/omnibox';
import {DisposableStack} from '../base/disposable_stack';
import {TraceInfo} from '../public/trace_info';
import {Workspace, WorkspaceManager} from '../public/workspace';
import {Migrate, Store} from '../base/store';
import {LegacyDetailsPanel} from '../public/details_panel';
import {scrollTo, ScrollToArgs} from '../public/scroll_helper';

// Every plugin gets its own PluginContext. This is how we keep track
// what each plugin is doing and how we can blame issues on particular
// plugins.
// The PluginContext exists for the whole duration a plugin is active.
export class PluginContextImpl implements App, Disposable {
  private trash = new DisposableStack();
  private alive = true;
  readonly commands;
  readonly sidebar;
  readonly omnibox;

  constructor(readonly pluginId: string) {
    const thiz = this;
    this.commands = {
      registerCommand(cmd: Command): void {
        // Silently ignore if context is dead.
        if (!thiz.alive) return;

        const disposable = globals.commandManager.registerCommand(cmd);
        thiz.trash.use(disposable);
      },
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      runCommand(id: string, ...args: any[]): any {
        return globals.commandManager.runCommand(id, ...args);
      },
      hasCommand(commandId: string) {
        return globals.commandManager.hasCommand(commandId);
      },
    };

    this.sidebar = {
      addMenuItem(menuItem: SidebarMenuItem): void {
        thiz.trash.use(globals.sidebarMenuItems.register(menuItem));
      },
    };

    this.omnibox = {
      prompt(text: string, options?: PromptOption[]) {
        return globals.omnibox.prompt(text, options);
      },
    };
  }

  [Symbol.dispose]() {
    this.trash.dispose();
    this.alive = false;
  }
}

// This PluginContextTrace implementation provides the plugin access to trace
// related resources, such as the engine and the store.
// The PluginContextTrace exists for the whole duration a plugin is active AND a
// trace is loaded.
class PluginContextTraceImpl implements Trace, Disposable {
  private trash = new DisposableStack();
  private alive = true;
  readonly commands;
  readonly engine: Engine;
  readonly sidebar;
  readonly tabs;
  readonly tracks;
  readonly omnibox;

  constructor(
    private ctx: App,
    engine: EngineBase,
  ) {
    const engineProxy = engine.getProxy(ctx.pluginId);
    this.trash.use(engineProxy);
    this.engine = engineProxy;
    const thiz = this;

    this.omnibox = ctx.omnibox;

    this.commands = {
      registerCommand(cmd: Command): void {
        // Silently ignore if context is dead.
        if (!thiz.alive) return;

        const dispose = globals.commandManager.registerCommand(cmd);
        thiz.trash.use(dispose);
      },

      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      runCommand(id: string, ...args: any[]): any {
        return ctx.commands.runCommand(id, ...args);
      },

      hasCommand(commandId: string) {
        return globals.commandManager.hasCommand(commandId);
      },
    };

    this.tracks = {
      registerTrack(trackDesc: TrackDescriptor): void {
        // Silently ignore if context is dead.
        if (!thiz.alive) return;

        const dispose = globals.trackManager.registerTrack({
          ...trackDesc,
          pluginId: thiz.pluginId,
        });
        thiz.trash.use(dispose);
      },
      findTrack(predicate: (desc: TrackDescriptor) => boolean | undefined) {
        return globals.trackManager.findTrack(predicate);
      },
      getAllTracks() {
        return globals.trackManager.getAllTracks();
      },
      getTrack(uri: string) {
        return globals.trackManager.getTrack(uri);
      },
    };

    this.tabs = {
      registerTab(desc: TabDescriptor): void {
        if (!thiz.alive) return;

        const unregister = globals.tabManager.registerTab(desc);
        thiz.trash.use(unregister);
      },

      addDefaultTab(uri: string): void {
        const remove = globals.tabManager.addDefaultTab(uri);
        thiz.trash.use(remove);
      },

      showTab(uri: string): void {
        globals.tabManager.showTab(uri);
      },

      hideTab(uri: string): void {
        globals.tabManager.hideTab(uri);
      },
    };

    this.sidebar = {
      addMenuItem(menuItem: SidebarMenuItem): void {
        // Silently ignore if context is dead.
        if (!thiz.alive) return;

        thiz.trash.use(globals.sidebarMenuItems.register(menuItem));
      },
    };
  }

  addQueryResultsTab(query: string, title: string) {
    addQueryResultsTab({query, title});
  }

  registerDetailsPanel(detailsPanel: LegacyDetailsPanel): void {
    if (!this.alive) return;

    const tabMan = globals.tabManager;
    const unregister = tabMan.registerLegacyDetailsPanel(detailsPanel);
    this.trash.use(unregister);
  }

  scrollTo(args: ScrollToArgs): void {
    scrollTo(args);
  }

  get pluginId(): string {
    return this.ctx.pluginId;
  }

  readonly timeline = {
    panToTimestamp(ts: time): void {
      globals.timeline.panToTimestamp(ts);
    },

    setViewportTime(start: time, end: time): void {
      globals.timeline.updateVisibleTime(new TimeSpan(start, end));
    },

    get visibleWindow() {
      return globals.timeline.visibleWindow;
    },
  };

  get workspace(): Workspace {
    return globals.workspace;
  }

  get selection() {
    return globals.selectionManager;
  }

  [Symbol.dispose]() {
    this.trash.dispose();
    this.alive = false;
  }

  mountStore<T>(migrate: Migrate<T>): Store<T> {
    return globals.store.createSubStore(['plugins', this.pluginId], migrate);
  }

  get workspaces(): WorkspaceManager {
    return globals.workspaceManager;
  }

  get traceInfo(): TraceInfo {
    return globals.traceContext;
  }

  get openerPluginArgs(): {[key: string]: unknown} | undefined {
    if (globals.state.engine?.source.type !== 'ARRAY_BUFFER') {
      return undefined;
    }
    const pluginArgs = globals.state.engine?.source.pluginArgs;
    return (pluginArgs ?? {})[this.pluginId];
  }
}

// 'Static' registry of all known plugins.
export class PluginRegistry extends Registry<PluginDescriptor> {
  constructor() {
    super((info) => info.pluginId);
  }
}

export interface PluginDetails {
  plugin: PerfettoPlugin;
  context: App & Disposable;
  traceContext?: PluginContextTraceImpl;
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
  private engine?: EngineBase;
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

    const context = new PluginContextImpl(id);

    plugin.onActivate?.(context);

    const pluginDetails: PluginDetails = {
      plugin,
      context,
    };

    // If a trace is already loaded when plugin is activated, make sure to
    // call onTraceLoad().
    if (this.engine) {
      await doPluginTraceLoad(pluginDetails, this.engine);
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

  isActive(pluginId: string): boolean {
    return this.getPluginContext(pluginId) !== undefined;
  }

  isEnabled(pluginId: string): boolean {
    return Boolean(this.flags.get(pluginId)?.get());
  }

  getPluginContext(pluginId: string): PluginDetails | undefined {
    return this._plugins.get(pluginId);
  }

  async onTraceLoad(
    engine: EngineBase,
    beforeEach?: (id: string) => void,
  ): Promise<void> {
    this.engine = engine;
    // Awaiting all plugins in parallel will skew timing data as later plugins
    // will spend most of their time waiting for earlier plugins to load.
    // Running in parallel will have very little performance benefit assuming
    // most plugins use the same engine, which can only process one query at a
    // time.
    for (const [id, plugin] of this._plugins.entries()) {
      beforeEach?.(id);
      await doPluginTraceLoad(plugin, engine);
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

  get needsRestart() {
    return this._needsRestart;
  }
}

async function doPluginTraceReady(pluginDetails: PluginDetails): Promise<void> {
  const {plugin, traceContext} = pluginDetails;
  await Promise.resolve(plugin.onTraceReady?.(assertExists(traceContext)));
  raf.scheduleFullRedraw();
}

async function doPluginTraceLoad(
  pluginDetails: PluginDetails,
  engine: EngineBase,
): Promise<void> {
  const {plugin, context} = pluginDetails;

  const traceCtx = new PluginContextTraceImpl(context, engine);
  pluginDetails.traceContext = traceCtx;

  const startTime = performance.now();
  await Promise.resolve(plugin.onTraceLoad?.(traceCtx));
  const loadTime = performance.now() - startTime;
  pluginDetails.previousOnTraceLoadTimeMillis = loadTime;

  raf.scheduleFullRedraw();
}

async function doPluginTraceUnload(
  pluginDetails: PluginDetails,
): Promise<void> {
  const {traceContext, plugin} = pluginDetails;

  if (traceContext) {
    plugin.onTraceUnload && (await plugin.onTraceUnload(traceContext));
    traceContext[Symbol.dispose]();
    pluginDetails.traceContext = undefined;
  }
}

// TODO(hjd): Sort out the story for global singletons like these:
export const pluginRegistry = new PluginRegistry();
export const pluginManager = new PluginManager(pluginRegistry);
