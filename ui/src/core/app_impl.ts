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

import {AsyncLimiter} from '../base/async_limiter';
import {defer} from '../base/deferred';
import {assertExists, assertTrue} from '../base/logging';
import {ServiceWorkerController} from '../frontend/service_worker_controller';
import {App} from '../public/app';
import {SqlPackage} from '../public/extra_sql_packages';
import {FeatureFlagManager, FlagSettings} from '../public/feature_flag';
import {Raf} from '../public/raf';
import {RouteArgs} from '../public/route_schema';
import {Setting} from '../public/settings';
import {TraceStream} from '../public/stream';
import {DurationPrecision, TimestampFormat} from '../public/timeline';
import {NewEngineMode} from '../trace_processor/engine';
import {AnalyticsInternal, initAnalytics} from './analytics_impl';
import {CommandInvocation, CommandManagerImpl} from './command_manager';
import {featureFlags} from './feature_flags';
import {loadTrace} from './load_trace';
import {OmniboxManagerImpl} from './omnibox_manager';
import {PageManagerImpl} from './page_manager';
import {PerfManager} from './perf_manager';
import {PluginManagerImpl} from './plugin_manager';
import {raf} from './raf_scheduler';
import {Router} from './router';
import {SettingsManagerImpl} from './settings_manager';
import {SidebarManagerImpl} from './sidebar_manager';
import {SerializedAppState} from './state_serialization_schema';
import {TraceImpl} from './trace_impl';
import {TraceArrayBufferSource, TraceSource} from './trace_source';

export type OpenTraceArrayBufArgs = Omit<
  Omit<TraceArrayBufferSource, 'type'>,
  'serializedAppState'
>;

// The args that frontend/index.ts passes when calling AppImpl.initialize().
// This is to deal with injections that would otherwise cause circular deps.
export interface AppInitArgs {
  readonly initialRouteArgs: RouteArgs;
  readonly settingsManager: SettingsManagerImpl;
  readonly timestampFormatSetting: Setting<TimestampFormat>;
  readonly durationPrecisionSetting: Setting<DurationPrecision>;
  readonly timezoneOverrideSetting: Setting<string>;
  readonly analyticsSetting: Setting<boolean>;
  readonly startupCommandsSetting: Setting<CommandInvocation[]>;
  readonly enforceStartupCommandAllowlistSetting: Setting<boolean>;
}

/**
 * Handles the global state of the ui, for anything that is not related to a
 * specific trace. This is always available even before a trace is loaded (in
 * contrast to TraceContext, which is bound to the lifetime of a trace).
 * There is only one instance in total of this class (see instance()).
 * This class is only exposed to TraceImpl, nobody else should refer to this
 * and should use AppImpl instead.
 */
export class AppImpl implements App {
  readonly commands = new CommandManagerImpl();
  readonly omnibox = new OmniboxManagerImpl();
  readonly pages = new PageManagerImpl();
  readonly sidebar: SidebarManagerImpl;
  readonly plugins = new PluginManagerImpl();
  readonly perfDebugging = new PerfManager();
  readonly analytics: AnalyticsInternal;
  readonly serviceWorkerController = new ServiceWorkerController();
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
  readonly settings: SettingsManagerImpl;

  // The current active trace (if any).
  private _activeTrace: TraceImpl | undefined;

  // This is normally empty and is injected with extra google-internal packages
  // via is_internal_user.js
  extraSqlPackages: SqlPackage[] = [];

  // This is normally empty and is injected with Base64-encoded protobuf
  // descriptor sets via is_internal_user.js.
  extraParsingDescriptors: string[] = [];

  // This is normally empty and is injected with extra google-internal macros
  // via is_internal_user.js
  extraMacros: Record<string, CommandInvocation[]>[] = [];

  // Promise which is resolved when extra loading is completed.
  extrasLoadingDeferred = defer<undefined>();

  // Initializes the singleton instance - must be called only once and before
  // AppImpl.instance is used.
  static initialize(initArgs: AppInitArgs): AppImpl {
    assertTrue(AppImpl._instance === undefined);
    AppImpl._instance = new AppImpl(initArgs);
    return AppImpl._instance;
  }

  // Singleton.
  private static _instance: AppImpl;
  static get instance(): AppImpl {
    return assertExists(AppImpl._instance);
  }

  readonly timestampFormat: Setting<TimestampFormat>;
  readonly durationPrecision: Setting<DurationPrecision>;
  readonly timezoneOverride: Setting<string>;
  readonly startupCommandsSetting: Setting<CommandInvocation[]>;
  readonly enforceStartupCommandAllowlistSetting: Setting<boolean>;
  private _isInternalUser?: boolean;

  // This constructor is invoked only once, when frontend/index.ts invokes
  // AppMainImpl.initialize().
  private constructor(initArgs: AppInitArgs) {
    this.timestampFormat = initArgs.timestampFormatSetting;
    this.durationPrecision = initArgs.durationPrecisionSetting;
    this.timezoneOverride = initArgs.timezoneOverrideSetting;
    this.startupCommandsSetting = initArgs.startupCommandsSetting;
    this.enforceStartupCommandAllowlistSetting =
      initArgs.enforceStartupCommandAllowlistSetting;
    this.settings = initArgs.settingsManager;
    this.initArgs = initArgs;
    this.initialRouteArgs = initArgs.initialRouteArgs;
    this.embeddedMode = this.initialRouteArgs.mode === 'embedded';
    this.testingMode =
      self.location !== undefined &&
      self.location.search.indexOf('testing=1') >= 0;
    this.sidebar = new SidebarManagerImpl({
      disabled: this.embeddedMode,
      hidden: this.initialRouteArgs.hideSidebar,
    });
    this.analytics = initAnalytics(
      this.testingMode,
      this.embeddedMode,
      initArgs.analyticsSetting.get(),
    );
  }

  setActiveTrace(trace: TraceImpl) {
    this.closeCurrentTrace();
    this._activeTrace = trace;
  }

  closeCurrentTrace() {
    this.omnibox.reset(/* focus= */ false);

    if (this._activeTrace) {
      // This will trigger the unregistration of trace-scoped commands and
      // sidebar menuitems (and few similar things).
      this._activeTrace[Symbol.dispose]();
      this._activeTrace = undefined;
    }
  }

  get isInternalUser() {
    if (this._isInternalUser === undefined) {
      this._isInternalUser = localStorage.getItem('isInternalUser') === '1';
    }
    return this._isInternalUser;
  }

  set isInternalUser(value: boolean) {
    localStorage.setItem('isInternalUser', value ? '1' : '0');
    this._isInternalUser = value;
    raf.scheduleFullRedraw();
  }

  get trace(): TraceImpl | undefined {
    return this._activeTrace;
  }

  get raf(): Raf {
    return raf;
  }

  get featureFlags(): FeatureFlagManager {
    return {
      register: (settings: FlagSettings) => featureFlags.register(settings),
    };
  }

  openTraceFromFile(file: File) {
    return this.openTrace({type: 'FILE', file});
  }

  openTraceFromMultipleFiles(files: ReadonlyArray<File>) {
    return this.openTrace({type: 'MULTIPLE_FILES', files});
  }

  openTraceFromUrl(url: string, serializedAppState?: SerializedAppState) {
    return this.openTrace({type: 'URL', url, serializedAppState});
  }

  openTraceFromStream(stream: TraceStream) {
    return this.openTrace({type: 'STREAM', stream});
  }

  openTraceFromBuffer(
    args: OpenTraceArrayBufArgs,
    serializedAppState?: SerializedAppState,
  ) {
    return this.openTrace({...args, type: 'ARRAY_BUFFER', serializedAppState});
  }

  openTraceFromHttpRpc() {
    return this.openTrace({type: 'HTTP_RPC'});
  }

  private async openTrace(src: TraceSource): Promise<TraceImpl> {
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
        src = {...src, buffer: src.buffer.buffer};
      } else {
        src = {...src, buffer: src.buffer.slice().buffer};
      }
    }

    const result = defer<TraceImpl>();

    // Rationale for asyncLimiter: openTrace takes several seconds and involves
    // a long sequence of async tasks (e.g. invoking plugins' onLoad()). These
    // tasks cannot overlap if the user opens traces in rapid succession, as
    // they will mess up the state of registries. So once we start, we must
    // complete trace loading (we don't bother supporting cancellations. If the
    // user is too bothered, they can reload the tab).
    await this.openTraceAsyncLimiter.schedule(async () => {
      // Wait for extras parsing descriptors to be loaded
      // via is_internal_user.js. This prevents a race condition where
      // trace loading would otherwise begin before this data is available.
      await this.extraLoadingPromise;
      this.closeCurrentTrace();
      this.isLoadingTrace = true;
      try {
        // loadTrace() in trace_loader.ts will do the following:
        // - Create a new engine.
        // - Pump the data from the TraceSource into the engine.
        // - Do the initial queries to build the TraceImpl object
        // - Call AppImpl.setActiveTrace(TraceImpl)
        // - Continue with the trace loading logic (track decider, plugins, etc)
        // - Resolve the promise when everything is done.
        const trace = await loadTrace(this, src);
        this.omnibox.reset(/* focus= */ false);
        // loadTrace() internally will call setActiveTrace() and change our
        // _currentTrace in the middle of its ececution. We cannot wait for
        // loadTrace to be finished before setting it because some internal
        // implementation details of loadTrace() rely on that trace to be current
        // to work properly (mainly the router hash uuid).

        result.resolve(trace);
      } catch (error) {
        result.reject(error);
      } finally {
        this.isLoadingTrace = false;
        raf.scheduleFullRedraw();
      }
    });

    return result;
  }

  navigate(newHash: string): void {
    Router.navigate(newHash);
  }

  notifyOnExtrasLoadingCompleted() {
    this.extrasLoadingDeferred.resolve();
  }

  get extraLoadingPromise(): Promise<undefined> {
    return this.extrasLoadingDeferred;
  }
}
