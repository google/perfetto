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
import {TraceImpl} from './trace_impl';
import {CommandManagerImpl} from './command_manager';
import {OmniboxManagerImpl} from './omnibox_manager';
import {raf} from './raf_scheduler';
import {SidebarManagerImpl} from './sidebar_manager';
import {PluginManager} from './plugin_manager';
import {NewEngineMode} from '../trace_processor/engine';
import {RouteArgs} from '../public/route_schema';
import {SqlPackage} from '../public/extra_sql_packages';

// The pseudo plugin id used for the core instance of AppImpl.

export const CORE_PLUGIN_ID = '__core__';

// The args that frontend/index.ts passes when calling AppImpl.initialize().
// This is to deal with injections that would otherwise cause circular deps.
export interface AppInitArgs {
  rootUrl: string;
  initialRouteArgs: RouteArgs;

  // TODO(primiano): remove once State is gone.
  // This maps to globals.dispatch(Actions.clearState({})),
  clearState: () => void;
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
  readonly commandMgr = new CommandManagerImpl();
  readonly omniboxMgr = new OmniboxManagerImpl();
  readonly sidebarMgr = new SidebarManagerImpl();
  readonly pluginMgr: PluginManager;
  newEngineMode: NewEngineMode = 'USE_HTTP_RPC_IF_AVAILABLE';
  initialRouteArgs: RouteArgs;
  isLoadingTrace = false; // Set when calling openTrace().
  readonly initArgs: AppInitArgs;

  // This is normally empty and is injected with extra google-internal packages
  // via is_internal_user.js
  extraSqlPackages: SqlPackage[] = [];

  // This constructor is invoked only once, when frontend/index.ts invokes
  // AppMainImpl.initialize().
  constructor(initArgs: AppInitArgs) {
    this.initArgs = initArgs;
    this.initialRouteArgs = initArgs.initialRouteArgs;

    // The rootUrl should point to 'https://ui.perfetto.dev/v1.2.3/'. It's
    // allowed to be empty only in unittests, because there there is no bundle
    // hence no concrete root.
    assertTrue(this.initArgs.rootUrl !== '' || typeof jest !== 'undefined');
    this.pluginMgr = new PluginManager({
      forkForPlugin: (p) => AppImpl.instance.forkForPlugin(p),
      get trace() {
        return AppImpl.instance.trace;
      },
    });
  }
}

/*
 * Every plugin gets its own instance. This is how we keep track
 * what each plugin is doing and how we can blame issues on particular
 * plugins.
 * The instance exists for the whole duration a plugin is active.
 */

export class AppImpl implements App {
  private appCtx: AppContext;
  readonly pluginId: string;
  private currentTrace?: TraceImpl;

  private constructor(appCtx: AppContext, pluginId: string) {
    this.appCtx = appCtx;
    this.pluginId = pluginId;
  }

  // Gets access to the one instance that the core can use. Note that this is
  // NOT the only instance, as other AppImpl instance will be created for each
  // plugin.
  private static _instance: AppImpl;

  // Invoked by frontend/index.ts.
  static initialize(args: AppInitArgs) {
    assertTrue(AppImpl._instance === undefined);
    AppImpl._instance = new AppImpl(new AppContext(args), CORE_PLUGIN_ID);
  }

  // For testing purposes only.
  // TODO(primiano): This is only required because today globals.ts abuses
  // createFakeTraceImpl(). It can be removed once globals goes away.
  static get initialized() {
    return AppImpl._instance !== undefined;
  }

  static get instance(): AppImpl {
    return assertExists(AppImpl._instance);
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

  get plugins(): PluginManager {
    return this.appCtx.pluginMgr;
  }

  get trace(): TraceImpl | undefined {
    return this.currentTrace;
  }

  scheduleRedraw(): void {
    raf.scheduleFullRedraw();
  }

  forkForPlugin(pluginId: string): AppImpl {
    assertTrue(pluginId != CORE_PLUGIN_ID);
    return new AppImpl(this.appCtx, pluginId);
  }

  get newEngineMode() {
    return this.appCtx.newEngineMode;
  }

  set newEngineMode(mode: NewEngineMode) {
    this.appCtx.newEngineMode = mode;
  }

  get initialRouteArgs(): RouteArgs {
    return this.appCtx.initialRouteArgs;
  }

  closeCurrentTrace() {
    // This method should be called only on the core instance, plugins don't
    // have access to openTrace*() methods.
    assertTrue(this.pluginId === CORE_PLUGIN_ID);
    this.omnibox.reset(/* focus= */ false);

    if (this.currentTrace !== undefined) {
      this.appCtx.pluginMgr.onTraceClose();
      // This will trigger the unregistration of trace-scoped commands and
      // sidebar menuitems (and few similar things).
      this.currentTrace[Symbol.dispose]();
      this.currentTrace = undefined;
    }
    this.appCtx.initArgs.clearState();
  }

  // Called by trace_loader.ts soon after it has created a new TraceImpl.
  setActiveTrace(traceImpl: TraceImpl) {
    // In 99% this closeCurrentTrace() call is not needed because the real one
    // is performed by openTrace() in this file. However in some rare cases we
    // might end up loading a trace while another one is still loading, and this
    // covers races in that case.
    this.closeCurrentTrace();
    this.currentTrace = traceImpl;
  }

  get isLoadingTrace() {
    return this.appCtx.isLoadingTrace;
  }

  // TODO(primiano): this is very temporary and will go away as soon as
  // TraceController is turned into an async function.
  setIsLoadingTrace(loading: boolean) {
    this.appCtx.isLoadingTrace = loading;
    raf.scheduleFullRedraw();
  }

  get rootUrl() {
    return this.appCtx.initArgs.rootUrl;
  }

  get extraSqlPackages(): SqlPackage[] {
    return this.appCtx.extraSqlPackages;
  }

  // Nothing other than TraceImpl's constructor should ever refer to this.
  // This is necessary to avoid circular dependencies between trace_impl.ts
  // and app_impl.ts.
  get __appCtxForTraceImplCtor() {
    return this.appCtx;
  }
}
