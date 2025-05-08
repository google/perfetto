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

import {assertExists, assertTrue} from '../base/logging';
import {App} from '../public/app';
import {TraceContext, TraceImpl} from './trace_impl';
import {CommandManagerImpl} from './command_manager';
import {OmniboxManagerImpl} from './omnibox_manager';
import {raf} from './raf_scheduler';
import {SidebarManagerImpl} from './sidebar_manager';
import {PluginManagerImpl} from './plugin_manager';
import {NewEngineMode} from '../trace_processor/engine';
import {RouteArgs} from '../public/route_schema';
import {SqlPackage} from '../public/extra_sql_packages';
import {SerializedAppState} from './state_serialization_schema';
import {PostedTrace, TraceSource} from './trace_source';
import {loadTrace} from './load_trace';
import {CORE_PLUGIN_ID} from './plugin_manager';
import {Router} from './router';
import {AnalyticsInternal, initAnalytics} from './analytics_impl';
import {createProxy, getOrCreate} from '../base/utils';
import {PageManagerImpl} from './page_manager';
import {PageHandler} from '../public/page';
import {PerfManager} from './perf_manager';
import {ServiceWorkerController} from '../frontend/service_worker_controller';
import {FeatureFlagManager, FlagSettings} from '../public/feature_flag';
import {featureFlags} from './feature_flags';
import {Raf} from '../public/raf';
import {AsyncLimiter} from '../base/async_limiter';
import {
  PERFETTO_SETTINGS_STORAGE_KEY,
  SettingsManagerImpl,
} from './settings_manager';
import {SettingsManager} from '../public/settings';
import {LocalStorage} from './local_storage';

// The args that frontend/index.ts passes when calling AppImpl.initialize().
// This is to deal with injections that would otherwise cause circular deps.
export interface AppInitArgs {
  initialRouteArgs: RouteArgs;
}

/**
 * Handles the global state of the ui, for anything that is not related to a
 * specific trace. This is always available even before a trace is loaded (in
 * contrast to TraceContext, which is bound to the lifetime of a trace).
 * There is only one instance in total of this class (see instance()).
 * This class is only exposed to TraceImpl, nobody else should refer to this
 * and should use AppImpl instead.
 */
export class AppContext {
  // The per-plugin instances of AppImpl (including the CORE_PLUGIN one).
  private readonly pluginInstances = new Map<string, AppImpl>();
  readonly commandMgr = new CommandManagerImpl();
  readonly omniboxMgr = new OmniboxManagerImpl();
  readonly pageMgr = new PageManagerImpl();
  readonly sidebarMgr: SidebarManagerImpl;
  readonly pluginMgr: PluginManagerImpl;
  readonly perfMgr = new PerfManager();
  readonly analytics: AnalyticsInternal;
  readonly serviceWorkerController: ServiceWorkerController;
  httpRpc = {
    newEngineMode: 'USE_HTTP_RPC_IF_AVAILABLE' as NewEngineMode,
    httpRpcAvailable: false,
  };
  initialRouteArgs: RouteArgs;
  isLoadingTrace = false; // Set when calling openTrace().
  readonly initArgs: AppInitArgs;
  readonly embeddedMode: boolean;
  readonly testingMode: boolean;
  readonly openTraceAsyncLimiter = new AsyncLimiter();
  readonly settingsManager = new SettingsManagerImpl(
    new LocalStorage(PERFETTO_SETTINGS_STORAGE_KEY),
  );

  // This is normally empty and is injected with extra google-internal packages
  // via is_internal_user.js
  extraSqlPackages: SqlPackage[] = [];

  // The currently open trace.
  currentTrace?: TraceContext;

  private static _instance: AppContext;

  static initialize(initArgs: AppInitArgs): AppContext {
    assertTrue(AppContext._instance === undefined);
    return (AppContext._instance = new AppContext(initArgs));
  }

  static get instance(): AppContext {
    return assertExists(AppContext._instance);
  }

  // This constructor is invoked only once, when frontend/index.ts invokes
  // AppMainImpl.initialize().
  private constructor(initArgs: AppInitArgs) {
    this.initArgs = initArgs;
    this.initialRouteArgs = initArgs.initialRouteArgs;
    this.serviceWorkerController = new ServiceWorkerController();
    this.embeddedMode = this.initialRouteArgs.mode === 'embedded';
    this.testingMode =
      self.location !== undefined &&
      self.location.search.indexOf('testing=1') >= 0;
    this.sidebarMgr = new SidebarManagerImpl({
      disabled: this.embeddedMode,
      hidden: this.initialRouteArgs.hideSidebar,
    });
    this.analytics = initAnalytics(this.testingMode, this.embeddedMode);
    this.pluginMgr = new PluginManagerImpl({
      forkForPlugin: (pluginId) => this.forPlugin(pluginId),
      get trace() {
        return AppImpl.instance.trace;
      },
    });
  }

  // Gets or creates an instance of AppImpl backed by the current AppContext
  // for the given plugin.
  forPlugin(pluginId: string) {
    return getOrCreate(this.pluginInstances, pluginId, () => {
      return new AppImpl(this, pluginId);
    });
  }

  closeCurrentTrace() {
    this.omniboxMgr.reset(/* focus= */ false);

    if (this.currentTrace !== undefined) {
      // This will trigger the unregistration of trace-scoped commands and
      // sidebar menuitems (and few similar things).
      this.currentTrace[Symbol.dispose]();
      this.currentTrace = undefined;
    }
  }

  // Called by trace_loader.ts soon after it has created a new TraceImpl.
  setActiveTrace(traceCtx: TraceContext) {
    // In 99% this closeCurrentTrace() call is not needed because the real one
    // is performed by openTrace() in this file. However in some rare cases we
    // might end up loading a trace while another one is still loading, and this
    // covers races in that case.
    this.closeCurrentTrace();
    this.currentTrace = traceCtx;
  }
}

/*
 * Every plugin gets its own instance. This is how we keep track
 * what each plugin is doing and how we can blame issues on particular
 * plugins.
 * The instance exists for the whole duration a plugin is active.
 */

export class AppImpl implements App {
  readonly pluginId: string;
  readonly initialPluginRouteArgs: {
    [key: string]: number | boolean | string;
  };
  private readonly appCtx: AppContext;
  private readonly pageMgrProxy: PageManagerImpl;

  // Invoked by frontend/index.ts.
  static initialize(args: AppInitArgs) {
    AppContext.initialize(args).forPlugin(CORE_PLUGIN_ID);
  }

  // Gets access to the one instance that the core can use. Note that this is
  // NOT the only instance, as other AppImpl instance will be created for each
  // plugin.
  static get instance(): AppImpl {
    return AppContext.instance.forPlugin(CORE_PLUGIN_ID);
  }

  // Only called by AppContext.forPlugin().
  constructor(appCtx: AppContext, pluginId: string) {
    this.appCtx = appCtx;
    this.pluginId = pluginId;

    const args: {[key: string]: string | number | boolean} = {};
    this.initialPluginRouteArgs = Object.entries(
      appCtx.initialRouteArgs,
    ).reduce((result, [key, value]) => {
      // Create a regex to match keys starting with pluginId
      const regex = new RegExp(`^${pluginId}:(.+)$`);
      const match = key.match(regex);

      // Only include entries that match the regex and have the right value type
      if (match && ['string', 'number', 'boolean'].includes(typeof value)) {
        const newKey = match[1];
        // Use the capture group (what comes after the prefix) as the new key
        result[newKey] = value;
      }
      return result;
    }, args);

    this.pageMgrProxy = createProxy(this.appCtx.pageMgr, {
      registerPage(pageHandler: PageHandler): Disposable {
        return appCtx.pageMgr.registerPage({
          ...pageHandler,
          pluginId,
        });
      },
    });
  }

  forPlugin(pluginId: string): AppImpl {
    return this.appCtx.forPlugin(pluginId);
  }

  get commands(): CommandManagerImpl {
    return this.appCtx.commandMgr;
  }

  get sidebar(): SidebarManagerImpl {
    return this.appCtx.sidebarMgr;
  }

  get omnibox(): OmniboxManagerImpl {
    return this.appCtx.omniboxMgr;
  }

  get plugins(): PluginManagerImpl {
    return this.appCtx.pluginMgr;
  }

  get analytics(): AnalyticsInternal {
    return this.appCtx.analytics;
  }

  get pages(): PageManagerImpl {
    return this.pageMgrProxy;
  }

  get trace(): TraceImpl | undefined {
    return this.appCtx.currentTrace?.forPlugin(this.pluginId);
  }

  get raf(): Raf {
    return raf;
  }

  get httpRpc() {
    return this.appCtx.httpRpc;
  }

  get initialRouteArgs(): RouteArgs {
    return this.appCtx.initialRouteArgs;
  }

  get settings(): SettingsManager {
    return this.appCtx.settingsManager;
  }

  get featureFlags(): FeatureFlagManager {
    return {
      register: (settings: FlagSettings) => featureFlags.register(settings),
    };
  }

  openTraceFromFile(file: File): void {
    this.openTrace({type: 'FILE', file});
  }

  openTraceFromUrl(url: string, serializedAppState?: SerializedAppState) {
    this.openTrace({type: 'URL', url, serializedAppState});
  }

  openTraceFromBuffer(postMessageArgs: PostedTrace): void {
    this.openTrace({type: 'ARRAY_BUFFER', ...postMessageArgs});
  }

  openTraceFromHttpRpc(): void {
    this.openTrace({type: 'HTTP_RPC'});
  }

  private async openTrace(src: TraceSource) {
    if (src.type === 'ARRAY_BUFFER' && src.buffer instanceof Uint8Array) {
      // Even though the type of `buffer` is ArrayBuffer, it's possible to
      // accidentally pass a Uint8Array here, because the interface of
      // Uint8Array is compatible with ArrayBuffer. That can cause subtle bugs
      // in TraceStream when creating chunks out of it (see b/390473162).
      // So if we get a Uint8Array in input, convert it into an actual
      // ArrayBuffer, as various parts of the codebase assume that this is a
      // pure ArrayBuffer, and not a logical view of it with a byteOffset > 0.
      if (
        src.buffer.byteOffset === 0 &&
        src.buffer.byteLength === src.buffer.buffer.byteLength
      ) {
        src.buffer = src.buffer.buffer;
      } else {
        src.buffer = src.buffer.slice().buffer;
      }
    }

    // Rationale for asyncLimiter: openTrace takes several seconds and involves
    // a long sequence of async tasks (e.g. invoking plugins' onLoad()). These
    // tasks cannot overlap if the user opens traces in rapid succession, as
    // they will mess up the state of registries. So once we start, we must
    // complete trace loading (we don't bother supporting cancellations. If the
    // user is too bothered, they can reload the tab).
    this.appCtx.openTraceAsyncLimiter.schedule(async () => {
      this.appCtx.closeCurrentTrace();
      this.appCtx.isLoadingTrace = true;
      try {
        // loadTrace() in trace_loader.ts will do the following:
        // - Create a new engine.
        // - Pump the data from the TraceSource into the engine.
        // - Do the initial queries to build the TraceImpl object
        // - Call AppImpl.setActiveTrace(TraceImpl)
        // - Continue with the trace loading logic (track decider, plugins, etc)
        // - Resolve the promise when everything is done.
        await loadTrace(this, src);
        this.omnibox.reset(/* focus= */ false);
        // loadTrace() internally will call setActiveTrace() and change our
        // _currentTrace in the middle of its ececution. We cannot wait for
        // loadTrace to be finished before setting it because some internal
        // implementation details of loadTrace() rely on that trace to be current
        // to work properly (mainly the router hash uuid).
      } finally {
        this.appCtx.isLoadingTrace = false;
        raf.scheduleFullRedraw();
      }
    });
  }

  // Called by trace_loader.ts soon after it has created a new TraceImpl.
  setActiveTrace(traceImpl: TraceImpl) {
    this.appCtx.setActiveTrace(traceImpl.__traceCtxForApp);
  }

  closeCurrentTrace() {
    this.appCtx.closeCurrentTrace();
  }

  get embeddedMode(): boolean {
    return this.appCtx.embeddedMode;
  }

  get testingMode(): boolean {
    return this.appCtx.testingMode;
  }

  get isLoadingTrace() {
    return this.appCtx.isLoadingTrace;
  }

  get extraSqlPackages(): SqlPackage[] {
    return this.appCtx.extraSqlPackages;
  }

  get perfDebugging(): PerfManager {
    return this.appCtx.perfMgr;
  }

  get serviceWorkerController(): ServiceWorkerController {
    return this.appCtx.serviceWorkerController;
  }

  // Nothing other than TraceImpl's constructor should ever refer to this.
  // This is necessary to avoid circular dependencies between trace_impl.ts
  // and app_impl.ts.
  get __appCtxForTrace() {
    return this.appCtx;
  }

  navigate(newHash: string): void {
    Router.navigate(newHash);
  }
}
