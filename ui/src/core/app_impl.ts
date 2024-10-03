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

import {assertTrue} from '../base/logging';
import {App} from '../public/app';
import {TraceContext, TraceImpl} from './trace_impl';
import {CommandManagerImpl} from './command_manager';
import {OmniboxManagerImpl} from './omnibox_manager';
import {raf} from './raf_scheduler';
import {SidebarManagerImpl} from './sidebar_manager';
import {PluginManager} from './plugin_manager';
import {NewEngineMode} from '../trace_processor/engine';
import {RouteArgs} from './route_schema';
import {Optional} from '../base/utils';

// The pseudo plugin id used for the core instance of AppImpl.

export const CORE_PLUGIN_ID = '__core__';

/**
 * Handles the global state of the ui, for anything that is not related to a
 * specific trace. This is always available even before a trace is loaded (in
 * contrast to TraceContext, which is bound to the lifetime of a trace).
 * There is only one instance in total of this class (see instance()).
 * This class is not exposed to anybody. Both core and plugins should access
 * this via AppImpl.
 */
export class AppContext {
  readonly commandMgr = new CommandManagerImpl();
  readonly omniboxMgr = new OmniboxManagerImpl();
  readonly sidebarMgr = new SidebarManagerImpl();
  readonly pluginMgr: PluginManager;
  newEngineMode: NewEngineMode = 'USE_HTTP_RPC_IF_AVAILABLE';
  initialRouteArgs?: RouteArgs = undefined;

  // The most recently created trace context. Can be undefined before any trace
  // is loaded.
  private traceCtx?: TraceContext;

  // There is only one global instance, lazily initialized on the first call.
  private static _instance: AppContext;
  static get instance() {
    return (AppContext._instance = AppContext._instance ?? new AppContext());
  }

  private constructor() {
    this.pluginMgr = new PluginManager({
      forkForPlugin: (p) => AppImpl.instance.forkForPlugin(p),
      get trace() {
        return AppImpl.instance.trace;
      },
    });
  }

  get currentTraceCtx(): TraceContext | undefined {
    return this.traceCtx;
  }

  // Called by AppImpl.newTraceInstance().
  setActiveTrace(traceCtx: TraceContext | undefined) {
    if (this.traceCtx !== undefined) {
      // This will trigger the unregistration of trace-scoped commands and
      // sidebar menuitems (and few similar things).
      this.traceCtx[Symbol.dispose]();
    }
    this.traceCtx = traceCtx;
  }

  // This is set to true when calling loadTrace() and cleared when it resolves.
  isLoadingTrace = false;
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
  static get instance(): AppImpl {
    AppImpl._instance =
      AppImpl._instance ?? new AppImpl(AppContext.instance, CORE_PLUGIN_ID);
    return AppImpl._instance;
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

  closeCurrentTrace() {
    this.currentTrace = undefined;
    this.appCtx.setActiveTrace(undefined);
  }

  scheduleRedraw(): void {
    raf.scheduleFullRedraw();
  }

  setActiveTrace(traceImpl: TraceImpl, traceCtx: TraceContext) {
    this.appCtx.setActiveTrace(traceCtx);
    this.currentTrace = traceImpl;
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

  setInitialRouteArgs(args: RouteArgs) {
    this.appCtx.initialRouteArgs = args;
  }

  getAndClearInitialRouteArgs(): Optional<RouteArgs> {
    const args = this.appCtx.initialRouteArgs;
    this.appCtx.initialRouteArgs = {};
    return args;
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
}
