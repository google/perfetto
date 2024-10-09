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
import {assertTrue} from '../base/logging';
import {createStore, Migrate, Store} from '../base/store';
import {TimelineImpl} from './timeline';
import {Command} from '../public/command';
import {Trace} from '../public/trace';
import {ScrollToArgs, setScrollToFunction} from '../public/scroll_helper';
import {TraceInfo} from '../public/trace_info';
import {TrackDescriptor} from '../public/track';
import {EngineBase, EngineProxy} from '../trace_processor/engine';
import {CommandManagerImpl} from './command_manager';
import {NoteManagerImpl} from './note_manager';
import {OmniboxManagerImpl} from './omnibox_manager';
import {SearchManagerImpl} from './search_manager';
import {SelectionManagerImpl} from './selection_manager';
import {SidebarManagerImpl} from './sidebar_manager';
import {TabManagerImpl} from './tab_manager';
import {TrackManagerImpl} from './track_manager';
import {WorkspaceManagerImpl} from './workspace_manager';
import {SidebarMenuItem} from '../public/sidebar';
import {ScrollHelper} from './scroll_helper';
import {Selection, SelectionOpts} from '../public/selection';
import {SearchResult} from '../public/search';
import {PivotTableManager} from './pivot_table_manager';
import {FlowManager} from './flow_manager';
import {AppContext, AppImpl} from './app_impl';
import {PluginManager} from './plugin_manager';
import {ThreadDesc, ThreadMap} from '../public/threads';
import {RouteArgs} from '../public/route_schema';
import {CORE_PLUGIN_ID} from './plugin_manager';

/**
 * Handles the per-trace state of the UI
 * There is an instance of this class per each trace loaded, and typically
 * between 0 and 1 instances in total (% brief moments while we swap traces).
 * 90% of the app state live here, including the Engine.
 * This is the underlying storage for AppImpl, which instead has one instance
 * per trace per plugin.
 */
class TraceContext implements Disposable {
  readonly appCtx: AppContext;
  readonly engine: EngineBase;
  readonly omniboxMgr = new OmniboxManagerImpl();
  readonly searchMgr: SearchManagerImpl;
  readonly selectionMgr: SelectionManagerImpl;
  readonly tabMgr = new TabManagerImpl();
  readonly timeline: TimelineImpl;
  readonly traceInfo: TraceInfo;
  readonly trackMgr = new TrackManagerImpl();
  readonly workspaceMgr = new WorkspaceManagerImpl();
  readonly noteMgr = new NoteManagerImpl();
  readonly flowMgr: FlowManager;
  readonly pluginSerializableState = createStore<{[key: string]: {}}>({});
  readonly scrollHelper: ScrollHelper;
  readonly pivotTableMgr;
  readonly threads = new Map<number, ThreadDesc>();
  readonly trash = new DisposableStack();

  // List of errors that were encountered while loading the trace by the TS
  // code. These are on top of traceInfo.importErrors, which is a summary of
  // what TraceProcessor reports on the stats table at import time.
  readonly loadingErrors: string[] = [];

  constructor(gctx: AppContext, engine: EngineBase, traceInfo: TraceInfo) {
    this.appCtx = gctx;
    this.engine = engine;
    this.trash.use(engine);
    this.traceInfo = traceInfo;
    this.timeline = new TimelineImpl(traceInfo);

    this.scrollHelper = new ScrollHelper(
      this.traceInfo,
      this.timeline,
      this.workspaceMgr.currentWorkspace,
      this.trackMgr,
    );

    this.selectionMgr = new SelectionManagerImpl(
      this.engine,
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
        this.selectionMgr.clear();
      }
    };

    this.pivotTableMgr = new PivotTableManager(
      engine.getProxy('PivotTableManager'),
    );

    this.flowMgr = new FlowManager(
      engine.getProxy('FlowManager'),
      this.trackMgr,
      this.selectionMgr,
      () => this.workspaceMgr.currentWorkspace,
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
    if (switchToCurrentSelectionTab) {
      this.tabMgr.showCurrentSelectionTab();
    }

    if (selection.kind === 'area') {
      this.pivotTableMgr.setSelectionArea(selection);
    }

    this.flowMgr.updateFlows(selection);
  }

  private onResultStep(searchResult: SearchResult) {
    this.selectionMgr.selectSearchResult(searchResult);
  }

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
  private appImpl: AppImpl;
  private traceCtx: TraceContext;

  // This is not the original Engine base, rather an EngineProxy based on the
  // same engineBase.
  private engineProxy: EngineProxy;
  private trackMgrProxy: TrackManagerImpl;
  private commandMgrProxy: CommandManagerImpl;
  private sidebarProxy: SidebarManagerImpl;

  // This is called by TraceController when loading a new trace, soon after the
  // engine has been set up. It obtains a new TraceImpl for the core. From that
  // we can fork sibling instances (i.e. bound to the same TraceContext) for
  // the various plugins.
  static createInstanceForCore(
    appImpl: AppImpl,
    engine: EngineBase,
    traceInfo: TraceInfo,
  ): TraceImpl {
    const traceCtx = new TraceContext(
      appImpl.__appCtxForTraceImplCtor,
      engine,
      traceInfo,
    );
    const traceImpl = new TraceImpl(appImpl, traceCtx);
    return traceImpl;
  }

  private constructor(appImpl: AppImpl, ctx: TraceContext) {
    const pluginId = appImpl.pluginId;
    this.appImpl = appImpl;
    this.traceCtx = ctx;
    const traceUnloadTrash = ctx.trash;

    // Invalidate all the engine proxies when the TraceContext is destroyed.
    this.engineProxy = ctx.engine.getProxy(pluginId);
    traceUnloadTrash.use(this.engineProxy);

    // Intercept the registerTrack() method to inject the pluginId into tracks.
    this.trackMgrProxy = createProxy(ctx.trackMgr, {
      registerTrack(trackDesc: TrackDescriptor): Disposable {
        return ctx.trackMgr.registerTrack({...trackDesc, pluginId});
      },
    });

    // CommandManager is global. Here we intercept the registerCommand() because
    // we want any commands registered via the Trace interface to be
    // unregistered when the trace unloads (before a new trace is loaded) to
    // avoid ending up with duplicate commands.
    this.commandMgrProxy = createProxy(ctx.appCtx.commandMgr, {
      registerCommand(cmd: Command): Disposable {
        const disposable = appImpl.commands.registerCommand(cmd);
        traceUnloadTrash.use(disposable);
        return disposable;
      },
    });

    // Likewise, remove all trace-scoped sidebar entries when the trace unloads.
    this.sidebarProxy = createProxy(ctx.appCtx.sidebarMgr, {
      addMenuItem(menuItem: SidebarMenuItem): Disposable {
        const disposable = appImpl.sidebar.addMenuItem(menuItem);
        traceUnloadTrash.use(disposable);
        return disposable;
      },
    });

    // TODO(primiano): remove this injection once we plumb Trace everywhere.
    setScrollToFunction((x: ScrollToArgs) => ctx.scrollHelper.scrollTo(x));
  }

  scrollTo(where: ScrollToArgs): void {
    this.traceCtx.scrollHelper.scrollTo(where);
  }

  // Creates an instance of TraceImpl backed by the same TraceContext for
  // another plugin. This is effectively a way to "fork" the core instance and
  // create the N instances for plugins.
  forkForPlugin(pluginId: string) {
    assertTrue(pluginId != CORE_PLUGIN_ID);
    return new TraceImpl(this.appImpl.forkForPlugin(pluginId), this.traceCtx);
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

  get openerPluginArgs(): {[key: string]: unknown} | undefined {
    const traceSource = this.traceCtx.traceInfo.source;
    if (traceSource.type !== 'ARRAY_BUFFER') {
      return undefined;
    }
    const pluginArgs = traceSource.pluginArgs;
    return (pluginArgs ?? {})[this.pluginId];
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

  get traceInfo(): TraceInfo {
    return this.traceCtx.traceInfo;
  }

  get notes() {
    return this.traceCtx.noteMgr;
  }

  get pivotTable() {
    return this.traceCtx.pivotTableMgr;
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

  get threads(): ThreadMap {
    return this.traceCtx.threads;
  }

  setThreads(threadMap: ThreadMap) {
    this.traceCtx.threads.clear();
    threadMap.forEach((v, k) => this.traceCtx.threads.set(k, v));
  }

  // App interface implementation.

  get pluginId(): string {
    return this.appImpl.pluginId;
  }

  get commands(): CommandManagerImpl {
    return this.commandMgrProxy;
  }

  get sidebar(): SidebarManagerImpl {
    return this.sidebarProxy;
  }

  get omnibox(): OmniboxManagerImpl {
    return this.appImpl.omnibox;
  }

  get plugins(): PluginManager {
    return this.appImpl.plugins;
  }

  get initialRouteArgs(): RouteArgs {
    return this.appImpl.initialRouteArgs;
  }

  get rootUrl(): string {
    return this.appImpl.rootUrl;
  }

  scheduleRedraw(): void {
    this.appImpl.scheduleRedraw();
  }

  [Symbol.dispose]() {
    if (this.pluginId === CORE_PLUGIN_ID) {
      this.traceCtx[Symbol.dispose]();
    }
  }
}

// A convenience interface to inject the App in Mithril components.
export interface TraceImplAttrs {
  trace: TraceImpl;
}

// Allows to take an existing class instance (`target`) and override some of its
// methods via `overrides`. We use this for cases where we want to expose a
// "manager" (e.g. TrackManager, SidebarManager) to the plugins, but we want to
// override few of its methods (e.g. to inject the pluginId in the args).
function createProxy<T extends object>(target: T, overrides: Partial<T>): T {
  return new Proxy(target, {
    get: (target: T, prop: string | symbol, receiver) => {
      // If the property is overriden, use that; otherwise, use target
      const overrideValue = (overrides as {[key: symbol | string]: {}})[prop];
      if (overrideValue !== undefined) {
        return typeof overrideValue === 'function'
          ? overrideValue.bind(overrides)
          : overrideValue;
      }
      const baseValue = Reflect.get(target, prop, receiver);
      return typeof baseValue === 'function'
        ? baseValue.bind(target)
        : baseValue;
    },
  }) as T;
}
