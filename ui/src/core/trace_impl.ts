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

import {DisposableStack} from '../base/disposable_stack';
import {createStore, Migrate, Store} from '../base/store';
import {TimelineImpl} from './timeline';
import {Trace} from '../public/trace';
import {ScrollToArgs} from '../public/scroll_helper';
import {Track} from '../public/track';
import {EngineBase, EngineProxy} from '../trace_processor/engine';
import {CommandManagerImpl, parseUrlCommands} from './command_manager';
import {NoteManagerImpl} from './note_manager';
import {OmniboxManagerImpl} from './omnibox_manager';
import {SearchManagerImpl} from './search_manager';
import {SelectionManagerImpl} from './selection_manager';
import {SidebarManagerImpl} from './sidebar_manager';
import {TabManagerImpl} from './tab_manager';
import {TrackManagerImpl} from './track_manager';
import {WorkspaceManagerImpl} from './workspace_manager';
import {ScrollHelper} from './scroll_helper';
import {Selection, SelectionOpts} from '../public/selection';
import {SearchResult} from '../public/search';
import {FlowManager} from './flow_manager';
import {AppContext, AppImpl, OpenTraceArrayBufArgs} from './app_impl';
import {PluginManagerImpl} from './plugin_manager';
import {RouteArgs} from '../public/route_schema';
import {CORE_PLUGIN_ID} from './plugin_manager';
import {Analytics} from '../public/analytics';
import {getOrCreate} from '../base/utils';
import {fetchWithProgress} from '../base/http_utils';
import {TraceInfoImpl} from './trace_info_impl';
import {PageHandler} from '../public/page';
import {createProxy} from '../base/utils';
import {PageManagerImpl} from './page_manager';
import {FeatureFlagManager, FlagSettings} from '../public/feature_flag';
import {SerializedAppState} from './state_serialization_schema';
import {featureFlags} from './feature_flags';
import {PerfManager} from './perf_manager';
import {Evt, EvtSource} from '../base/events';
import {Raf} from '../public/raf';
import {StatusbarManagerImpl} from './statusbar_manager';
import {PerfettoPlugin, PerfettoPluginStatic} from '../public/plugin';
import {SettingsManager} from '../public/settings';
import {SettingsManagerImpl} from './settings_manager';
import {MinimapManagerImpl} from './minimap_manager';
import {isStartupCommandAllowed} from './startup_command_allowlist';

/**
 * Handles the per-trace state of the UI
 * There is an instance of this class per each trace loaded, and typically
 * between 0 and 1 instances in total (% brief moments while we swap traces).
 * 90% of the app state live here, including the Engine.
 * This is the underlying storage for AppImpl, which instead has one instance
 * per trace per plugin.
 */
export class TraceContext implements Disposable {
  private readonly pluginInstances = new Map<string, TraceImpl>();
  readonly appCtx: AppContext;
  readonly engine: EngineBase;
  readonly commandMgr: CommandManagerImpl;
  readonly pageMgr: PageManagerImpl;
  readonly sidebarMgr: SidebarManagerImpl;
  readonly settingsManager: SettingsManagerImpl;
  readonly pluginMgr: PluginManagerImpl;
  readonly searchMgr: SearchManagerImpl;
  readonly selectionMgr: SelectionManagerImpl;
  readonly tabMgr = new TabManagerImpl();
  readonly timeline: TimelineImpl;
  readonly traceInfo: TraceInfoImpl;
  readonly trackMgr = new TrackManagerImpl();
  readonly workspaceMgr = new WorkspaceManagerImpl();
  readonly noteMgr = new NoteManagerImpl();
  readonly flowMgr: FlowManager;
  readonly pluginSerializableState = createStore<{[key: string]: {}}>({});
  readonly scrollHelper: ScrollHelper;
  readonly trash = new DisposableStack();
  readonly onTraceReady = new EvtSource<void>();
  readonly statusbarMgr = new StatusbarManagerImpl();
  readonly minimapManager = new MinimapManagerImpl();

  // List of errors that were encountered while loading the trace by the TS
  // code. These are on top of traceInfo.importErrors, which is a summary of
  // what TraceProcessor reports on the stats table at import time.
  readonly loadingErrors: string[] = [];

  constructor(gctx: AppContext, engine: EngineBase, traceInfo: TraceInfoImpl) {
    this.appCtx = gctx;
    this.engine = engine;
    this.trash.use(engine);
    this.traceInfo = traceInfo;

    // Trace-scoped children of app-scoped managers
    this.commandMgr = gctx.commandMgr.createChild();
    this.pageMgr = gctx.pageMgr.createChild();
    this.sidebarMgr = gctx.sidebarMgr.createChild(engine.id);
    this.settingsManager = gctx.settingsManager.createChild();
    this.pluginMgr = gctx.pluginMgr.createChild();

    this.timeline = new TimelineImpl(
      traceInfo,
      this.appCtx.timestampFormat,
      this.appCtx.durationPrecision,
      this.appCtx.timezoneOverride,
    );

    this.scrollHelper = new ScrollHelper(
      this.traceInfo,
      this.timeline,
      this.workspaceMgr.currentWorkspace,
      this.trackMgr,
    );

    this.selectionMgr = new SelectionManagerImpl(
      this.engine,
      this.timeline,
      this.trackMgr,
      this.noteMgr,
      this.scrollHelper,
      this.onSelectionChange.bind(this),
    );

    this.noteMgr.onNoteDeleted = (noteId) => {
      if (
        this.selectionMgr.selection.kind === 'note' &&
        this.selectionMgr.selection.id === noteId
      ) {
        this.selectionMgr.clearSelection();
      }
    };

    this.flowMgr = new FlowManager(
      engine.getProxy('FlowManager'),
      this.trackMgr,
      this.selectionMgr,
    );

    this.searchMgr = new SearchManagerImpl({
      timeline: this.timeline,
      trackManager: this.trackMgr,
      engine: this.engine,
      workspace: this.workspaceMgr.currentWorkspace,
      onResultStep: this.onResultStep.bind(this),
    });
  }

  // This method wires up changes to selection to side effects on search and
  // tabs. This is to avoid entangling too many dependencies between managers.
  private onSelectionChange(selection: Selection, opts: SelectionOpts) {
    const {clearSearch = true, switchToCurrentSelectionTab = true} = opts;
    if (clearSearch) {
      this.searchMgr.reset();
    }
    if (switchToCurrentSelectionTab && selection.kind !== 'empty') {
      this.tabMgr.showCurrentSelectionTab();
    }

    this.flowMgr.updateFlows(selection);
  }

  private onResultStep(searchResult: SearchResult) {
    this.selectionMgr.selectSearchResult(searchResult);
  }

  // Gets or creates an instance of TraceImpl backed by the current TraceContext
  // for the given plugin.
  forPlugin(pluginId: string) {
    return getOrCreate(this.pluginInstances, pluginId, () => {
      const appForPlugin = this.appCtx.forPlugin(pluginId);
      return new TraceImpl(appForPlugin, this);
    });
  }

  // Called by AppContext.closeCurrentTrace().
  [Symbol.dispose]() {
    this.trash.dispose();
  }
}

/**
 * This implementation provides the plugin access to trace related resources,
 * such as the engine and the store. This exists for the whole duration a plugin
 * is active AND a trace is loaded.
 * There are N+1 instances of this for each trace, one for each plugin plus one
 * for the core.
 */
export class TraceImpl implements Trace {
  private readonly appImpl: AppImpl;
  private readonly traceCtx: TraceContext;

  // This is not the original Engine base, rather an EngineProxy based on the
  // same engineBase.
  private readonly engineProxy: EngineProxy;
  private readonly trackMgrProxy: TrackManagerImpl;
  private readonly commandMgrProxy: CommandManagerImpl;
  private readonly pageMgrProxy: PageManagerImpl;
  private readonly pluginMgrProxy: PluginManagerImpl;

  private readonly omniboxMgr: OmniboxManagerImpl;

  // This is called by TraceController when loading a new trace, soon after the
  // engine has been set up. It obtains a new TraceImpl for the core. From that
  // we can fork sibling instances (i.e. bound to the same TraceContext) for
  // the various plugins.
  static createInstanceForCore(
    appImpl: AppImpl,
    engine: EngineBase,
    traceInfo: TraceInfoImpl,
  ): TraceImpl {
    const traceCtx = new TraceContext(
      appImpl.__appCtxForTrace,
      engine,
      traceInfo,
    );
    return traceCtx.forPlugin(CORE_PLUGIN_ID);
  }

  // Only called by TraceContext.forPlugin().
  constructor(appImpl: AppImpl, ctx: TraceContext) {
    const pluginId = appImpl.pluginId;
    this.appImpl = appImpl;
    this.traceCtx = ctx;
    const traceUnloadTrash = ctx.trash;

    const childOmnibox = appImpl.omnibox.childFor(ctx);
    traceUnloadTrash.use(childOmnibox);
    this.omniboxMgr = childOmnibox;

    // Invalidate all the engine proxies when the TraceContext is destroyed.
    this.engineProxy = ctx.engine.getProxy(pluginId);
    traceUnloadTrash.use(this.engineProxy);

    // Intercept the registerTrack() method to inject the pluginId into tracks.
    this.trackMgrProxy = createProxy(ctx.trackMgr, {
      registerTrack(trackDesc: Track): Disposable {
        return ctx.trackMgr.registerTrack({...trackDesc, pluginId});
      },
    });

    // CRITICAL ORDER: URL commands MUST execute before settings commands!
    // This ordering has subtle but important implications:
    // - URL commands are trace-specific and should establish initial state
    // - Settings commands are user preferences that should override URL defaults
    // - Changing this order could break trace sharing and user customization
    // DO NOT REORDER without understanding the full impact!
    const urlCommands =
      parseUrlCommands(ctx.appCtx.initialRouteArgs.startupCommands) ?? [];
    const settingsCommands = ctx.appCtx.startupCommandsSetting.get();

    // Combine URL and settings commands - runtime allowlist checking will handle filtering
    const allStartupCommands = [...urlCommands, ...settingsCommands];
    const enforceAllowlist =
      ctx.appCtx.enforceStartupCommandAllowlistSetting.get();

    // Proxy our trace-scoped command manager to add the
    // start-up commands
    this.commandMgrProxy = createProxy(this.traceCtx.commandMgr, {
      hasStartupCommands(): boolean {
        return allStartupCommands.length > 0;
      },

      async runStartupCommands(): Promise<void> {
        // Execute startup commands in trace context after everything is ready.
        // This simulates user actions taken after trace load is complete,
        // including any saved app state restoration. At this point:
        // - All plugins have loaded and registered their commands
        // - Trace data is fully accessible
        // - UI state has been restored from any saved workspace
        // - Commands can safely query trace data and modify UI state

        // Set allowlist checking during startup if enforcement enabled
        if (enforceAllowlist) {
          ctx.appCtx.commandMgr.setAllowlistCheck(isStartupCommandAllowed);
        }

        try {
          for (const command of allStartupCommands) {
            try {
              // Execute through proxy to access both global and trace-specific
              // commands.
              await ctx.appCtx.commandMgr.runCommand(
                command.id,
                ...command.args,
              );
            } catch (error) {
              // TODO(stevegolton): Add a mechanism to notify users of startup
              // command errors. This will involve creating a notification UX
              // similar to VSCode where there are popups on the bottom right
              // of the UI.
              console.warn(`Startup command ${command.id} failed:`, error);
            }
          }
        } finally {
          // Always restore default (allow all) behavior when done
          ctx.appCtx.commandMgr.setAllowlistCheck(() => true);
        }
      },
    });

    // Inject the plugin ID into page registrations
    this.pageMgrProxy = createProxy(ctx.pageMgr, {
      registerPage(pageHandler: PageHandler): Disposable {
        return ctx.pageMgr.registerPage({
          ...pageHandler,
          pluginId: appImpl.pluginId,
        });
      },
    });

    // Default the trace to myself when plugins access other plugins
    const pluginMgr = ctx.pluginMgr;
    const defaultTrace = this;
    this.pluginMgrProxy = createProxy(pluginMgr, {
      getPlugin<T extends PerfettoPlugin>(
        pluginDescriptor: PerfettoPluginStatic<T>,
        trace?: Trace,
      ): T {
        return pluginMgr.getPlugin(pluginDescriptor, trace ?? defaultTrace);
      },
    });
  }

  scrollTo(where: ScrollToArgs): void {
    this.traceCtx.scrollHelper.scrollTo(where);
  }

  // Creates an instance of TraceImpl backed by the same TraceContext for
  // another plugin. This is effectively a way to "fork" the core instance and
  // create the N instances for plugins.
  forkForPlugin(pluginId: string) {
    return this.traceCtx.forPlugin(pluginId);
  }

  mountStore<T>(migrate: Migrate<T>): Store<T> {
    return this.traceCtx.pluginSerializableState.createSubStore(
      [this.pluginId],
      migrate,
    );
  }

  getPluginStoreForSerialization() {
    return this.traceCtx.pluginSerializableState;
  }

  async getTraceFile(): Promise<Blob> {
    const src = this.traceInfo.source;
    if (this.traceInfo.downloadable) {
      if (src.type === 'ARRAY_BUFFER') {
        return new Blob([src.buffer]);
      } else if (src.type === 'FILE') {
        return src.file;
      } else if (src.type === 'URL') {
        return await fetchWithProgress(src.url, (progressPercent: number) =>
          this.omnibox.showStatusMessage(
            `Downloading trace ${progressPercent}%`,
          ),
        );
      }
    }
    // Not available in HTTP+RPC mode. Rather than propagating an undefined,
    // show a graceful error (the ERR:trace_src will be intercepted by
    // error_dialog.ts). We expect all users of this feature to not be able to
    // do anything useful if we returned undefined (other than showing the same
    // dialog).
    // The caller was supposed to check that traceInfo.downloadable === true
    // before calling this. Throwing while downloadable is true is a bug.
    throw new Error(`Cannot getTraceFile(${src.type})`);
  }

  get openerPluginArgs(): {[key: string]: unknown} | undefined {
    const traceSource = this.traceCtx.traceInfo.source;
    if (traceSource.type !== 'ARRAY_BUFFER') {
      return undefined;
    }
    const pluginArgs = traceSource.pluginArgs;
    return (pluginArgs ?? {})[this.pluginId];
  }

  get trace() {
    return this;
  }

  get onActiveTraceChanged(): Evt<Trace | undefined> {
    return this.appImpl.onActiveTraceChanged;
  }

  get minimap() {
    return this.traceCtx.minimapManager;
  }

  get engine() {
    return this.engineProxy;
  }

  get timeline() {
    return this.traceCtx.timeline;
  }

  get tracks() {
    return this.trackMgrProxy;
  }

  get tabs() {
    return this.traceCtx.tabMgr;
  }

  get workspace() {
    return this.traceCtx.workspaceMgr.currentWorkspace;
  }

  get workspaces() {
    return this.traceCtx.workspaceMgr;
  }

  get search() {
    return this.traceCtx.searchMgr;
  }

  get selection() {
    return this.traceCtx.selectionMgr;
  }

  get traceInfo(): TraceInfoImpl {
    return this.traceCtx.traceInfo;
  }

  get statusbar(): StatusbarManagerImpl {
    return this.traceCtx.statusbarMgr;
  }

  get notes() {
    return this.traceCtx.noteMgr;
  }

  get flows() {
    return this.traceCtx.flowMgr;
  }

  get loadingErrors(): ReadonlyArray<string> {
    return this.traceCtx.loadingErrors;
  }

  addLoadingError(err: string) {
    this.traceCtx.loadingErrors.push(err);
  }

  // App interface implementation.

  get pluginId(): string {
    return this.appImpl.pluginId;
  }

  get commands(): CommandManagerImpl {
    return this.commandMgrProxy;
  }

  get sidebar(): SidebarManagerImpl {
    return this.traceCtx.sidebarMgr;
  }

  get pages(): PageManagerImpl {
    return this.pageMgrProxy;
  }

  get omnibox(): OmniboxManagerImpl {
    return this.omniboxMgr;
  }

  get plugins(): PluginManagerImpl {
    return this.pluginMgrProxy;
  }

  get analytics(): Analytics {
    return this.appImpl.analytics;
  }

  get initialRouteArgs(): RouteArgs {
    return this.appImpl.initialRouteArgs;
  }

  get initialPluginRouteArgs() {
    return this.appImpl.initialPluginRouteArgs;
  }

  get featureFlags(): FeatureFlagManager {
    return {
      register: (settings: FlagSettings) => featureFlags.register(settings),
    };
  }

  get raf(): Raf {
    return this.appImpl.raf;
  }

  navigate(newHash: string): void {
    this.appImpl.navigate(newHash);
  }

  openTraceFromFile(file: File): void {
    this.appImpl.openTraceFromFile(file);
  }

  openTraceFromUrl(url: string, serializedAppState?: SerializedAppState) {
    this.appImpl.openTraceFromUrl(url, serializedAppState);
  }

  openTraceFromBuffer(
    args: OpenTraceArrayBufArgs,
    serializedAppState?: SerializedAppState,
  ): void {
    this.appImpl.openTraceFromBuffer(args, serializedAppState);
  }

  closeTrace(trace: Trace): void {
    this.appImpl.closeTrace(trace);
  }

  get onTraceReady() {
    return this.traceCtx.onTraceReady;
  }

  get perfDebugging(): PerfManager {
    return this.appImpl.perfDebugging;
  }

  get trash(): DisposableStack {
    return this.traceCtx.trash;
  }

  // Nothing other than AppImpl should ever refer to this, hence the __ name.
  get __traceCtxForApp() {
    return this.traceCtx;
  }

  get settings(): SettingsManager {
    return this.traceCtx.settingsManager;
  }

  get isInternalUser(): boolean {
    return this.appImpl.isInternalUser;
  }
}

// A convenience interface to inject the App in Mithril components.
export interface TraceImplAttrs {
  trace: TraceImpl;
}

export interface OptionalTraceImplAttrs {
  trace?: TraceImpl;
}
