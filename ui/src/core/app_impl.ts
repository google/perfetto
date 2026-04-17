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
import {assertExists, assertTrue} from '../base/assert';
import {ServiceWorkerController} from '../frontend/service_worker_controller';
import {App} from '../public/app';
import {SqlPackage} from '../public/extra_sql_packages';
import {FeatureFlagManager, FlagSettings} from '../public/feature_flag';
import {Raf} from '../public/raf';
import {RouteArgs} from '../public/route_schema';
import {Setting} from '../public/settings';
import {TraceStream} from '../public/stream';
import {DurationPrecision, TimestampFormat} from '../public/timeline';
import {EngineBase, NewEngineMode} from '../trace_processor/engine';
import {AnalyticsInternal, initAnalytics} from './analytics_impl';
import {
  CommandInvocation,
  CommandManagerImpl,
  Macro,
  parseUrlCommands,
} from './command_manager';
import {featureFlags} from './feature_flags';
import {loadTrace, RawTrace} from './load_trace';
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
import {TaskTrackerImpl} from '../frontend/task_tracker/task_tracker';
import {Embedder} from './embedder/embedder';
import {createEmbedder} from './embedder/create_embedder';
import {
  deserializeAppStatePhase1,
  deserializeAppStatePhase2,
} from './state_serialization';
import {cacheTrace} from './cache_manager';
import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {base64Decode} from '../base/string_utils';
import {uuidv4} from '../base/uuid';

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
  readonly omnibox = new OmniboxManagerImpl();
  readonly commands = new CommandManagerImpl(this.omnibox);
  readonly pages: PageManagerImpl;
  readonly sidebar: SidebarManagerImpl;
  readonly plugins: PluginManagerImpl;
  readonly perfDebugging = new PerfManager();
  readonly analytics: AnalyticsInternal;
  readonly serviceWorkerController = new ServiceWorkerController();
  readonly taskTracker = new TaskTrackerImpl();
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
  readonly embedder: Embedder;

  // The current active trace (if any).
  private _activeTrace: TraceImpl | undefined;

  // Extra SQL packages injected from extensions.
  private _sqlPackagesPromises = new Array<
    Promise<ReadonlyArray<SqlPackage>>
  >();

  // Protobuf descriptor sets as Base64-encoded strings injected from extensions.
  private _protoDescriptorsPromises = new Array<
    Promise<ReadonlyArray<string>>
  >();

  // Command macros. Injected from extensions.
  private _macrosPromises = new Array<
    Promise<ReadonlyArray<Macro & {source?: string}>>
  >();

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
    this.embedder = createEmbedder();
    this.plugins = new PluginManagerImpl(this.embedder.defaultPlugins);
    this.analytics = initAnalytics(
      this.testingMode,
      this.embeddedMode,
      initArgs.analyticsSetting.get(),
      this.embedder.analyticsId,
    );
    this.pages = new PageManagerImpl(this.analytics);
  }

  renderPageForCurrentRoute() {
    if (this.trace) {
      return this.trace.pages.renderPageForCurrentRoute();
    } else {
      return this.pages.renderPageForCurrentRoute();
    }
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

  setIsInternalUser(promise: Promise<boolean>) {
    promise.then((value) => {
      this._isInternalUser = value;
      localStorage.setItem('isInternalUser', value ? '1' : '0');
      raf.scheduleFullRedraw();
    });
  }

  get trace(): TraceImpl | undefined {
    // Parse the doc out of the location hash
    const currentRoute = Router.parseFragment(location.hash);
    const docUuid = currentRoute.args.doc as string;
    return this.traces.get(docUuid);
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

  // A map of loaded traces by document key
  traces = new Map<string, TraceImpl>();

  private async openTrace(src: TraceSource): Promise<TraceImpl> {
    const result = defer<TraceImpl>();
    const documentUuid = uuidv4();

    // Update the URL bar to the new document ID
    document.location.hash = '#!/?doc=' + documentUuid;

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
      this.closeCurrentTrace();
      this.isLoadingTrace = true;
      try {
        const extraParsingDescriptors: Uint8Array[] = [];
        for (const b64Str of await this.protoDescriptors()) {
          extraParsingDescriptors.push(base64Decode(b64Str));
        }

        const useHttpIfAvailable =
          this.httpRpc.newEngineMode === 'USE_HTTP_RPC_IF_AVAILABLE';

        // loadTrace() in trace_loader.ts will do the following:
        // - Create a new engine.
        // - Pump the data from the TraceSource into the engine.
        // - Do the initial queries to build the TraceImpl object
        // - Call AppImpl.setActiveTrace(TraceImpl)
        // - Continue with the trace loading logic (track decider, plugins, etc)
        // - Resolve the promise when everything is done.
        const rawTrace = await loadTrace(src, {
          useHttpIfAvailable,
          extraParsingDescriptors,
        });

        // Try to cache the trace bytes if possible
        const cached = await cacheTrace(src, rawTrace.uuid);

        const trace = new TraceImpl(this, rawTrace.engine, {
          source: src,
          cached: cached,
          uuid: rawTrace.uuid,
          start: rawTrace.traceSpan.start,
          end: rawTrace.traceSpan.end,
          tzOffMin: rawTrace.tzOffMin,
          unixOffset: rawTrace.unixOffset,
          traceTypes: rawTrace.traceTypes,
          hasFtrace: rawTrace.hasFtrace,
          traceTitle: rawTrace.traceTitle,
          traceUrl: rawTrace.traceUrl,
          downloadable: rawTrace.downloadable,
          importErrors: rawTrace.importErrors,
        });

        this.setActiveTrace(trace);
        await this.initializeTrace(rawTrace, trace, src);
        this.omnibox.reset(/* focus= */ false);

        this.traces.set(documentUuid, trace);

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

  addSqlPackages(
    args: ReadonlyArray<SqlPackage> | Promise<ReadonlyArray<SqlPackage>>,
  ) {
    this._sqlPackagesPromises.push(Promise.resolve(args));
  }

  async sqlPackages(): Promise<ReadonlyArray<SqlPackage>> {
    return Promise.all(this._sqlPackagesPromises).then((pkgs) =>
      pkgs.flatMap((p) => p),
    );
  }

  addProtoDescriptors(
    args: ReadonlyArray<string> | Promise<ReadonlyArray<string>>,
  ) {
    this._protoDescriptorsPromises.push(Promise.resolve(args));
  }

  async protoDescriptors(): Promise<ReadonlyArray<string>> {
    return Promise.all(this._protoDescriptorsPromises).then((desc) =>
      desc.flatMap((d) => d),
    );
  }

  addMacros(
    args:
      | ReadonlyArray<Macro & {source?: string}>
      | Promise<ReadonlyArray<Macro & {source?: string}>>,
  ) {
    this._macrosPromises.push(Promise.resolve(args));
  }

  async macros(): Promise<ReadonlyArray<Macro & {source?: string}>> {
    const macrosArray = await Promise.all(this._macrosPromises);
    return macrosArray.flat();
  }

  async initializeTrace(
    rawTrace: RawTrace,
    trace: TraceImpl,
    traceSource: TraceSource,
  ): Promise<TraceImpl> {
    const engine = rawTrace.engine;

    await this.installSqlPackages(engine);

    if (traceSource.serializedAppState !== undefined) {
      deserializeAppStatePhase1(traceSource.serializedAppState, trace);
    }

    // Initialize the plugins (call onTraceLoad() on all plugins)
    await this.plugins.onTraceLoad(trace, (id) => {
      this.updateStatus(`Running plugin: ${id}`);
    });

    // Decide which tabls to show on startup
    this.decideTabs(trace);

    // Load the minimap after pluigns have had a chance to register their loaders
    this.updateStatus(`Loading minimap`);
    await trace.minimap.load(rawTrace.traceSpan.start, rawTrace.traceSpan.end);

    // // Trace Processor doesn't support the reliable range feature for JSON
    // // traces.
    // if (!hasJsonTrace && ENABLE_CHROME_RELIABLE_RANGE_ANNOTATION_FLAG.get()) {
    //   const reliableRangeStart = await computeTraceReliableRangeStart(engine);
    //   if (reliableRangeStart > 0) {
    //     trace.notes.addNote({
    //       timestamp: reliableRangeStart,
    //       color: '#ff0000',
    //       text: 'Reliable Range Start',
    //     });
    //   }
    // }

    // notify() will await that all listeners' promises have resolved.
    await trace.onTraceReady.notify();

    if (traceSource.serializedAppState !== undefined) {
      // Wait that plugins have completed their actions and then proceed with
      // the final phase of app state restore.
      // TODO(primiano): this can probably be removed once we refactor tracks
      // to be URI based and can deal with non-existing URIs.
      deserializeAppStatePhase2(traceSource.serializedAppState, trace);
    }

    // Update the timeline to show the reliable range of the trace
    trace.timeline.setVisibleWindow(
      HighPrecisionTimeSpan.fromTime(
        rawTrace.reliableRange.start,
        rawTrace.reliableRange.end,
      ),
    );

    // Execute startup commands as the final step - simulates user actions
    // after the trace is fully loaded and any saved state has been restored.
    // This ensures startup commands see the complete, final state of the trace.
    await this.runStartupCommands(trace);

    return trace;
  }

  private async runStartupCommands(trace: TraceImpl) {
    // CRITICAL ORDER: URL commands MUST execute before settings commands!
    // This ordering has subtle but important implications:
    // - URL commands are trace-specific and should establish initial state
    // - Settings commands are user preferences that should override URL defaults
    // - Changing this order could break trace sharing and user customization
    // DO NOT REORDER without understanding the full impact!
    const urlCommands =
      parseUrlCommands(this.initialRouteArgs.startupCommands) ?? [];
    const settingsCommands = this.startupCommandsSetting.get();

    // Combine URL and settings commands - runtime allowlist checking will handle filtering
    const allStartupCommands = [...urlCommands, ...settingsCommands];
    const enforceAllowlist = this.enforceStartupCommandAllowlistSetting.get();

    if (allStartupCommands.length > 0) {
      this.updateStatus('Running startup commands');
      using _ = trace.omnibox.disablePrompts();

      // Execute startup commands in trace context after everything is ready.
      // This simulates user actions taken after trace load is complete,
      // including any saved app state restoration. At this point:
      // - All plugins have loaded and registered their commands
      // - Trace data is fully accessible
      // - UI state has been restored from any saved workspace
      // - Commands can safely query trace data and modify UI state
      // Set allowlist checking during startup if enforcement enabled
      if (enforceAllowlist) {
        this.commands.setExecutingStartupCommands(true);
      }

      try {
        for (const command of allStartupCommands) {
          try {
            // Execute through proxy to access both global and trace-specific
            // commands.
            await this.commands.runCommand(command.id, ...command.args);
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
        this.commands.setExecutingStartupCommands(false);
      }
    }
  }

  private updateStatus(message: string) {
    this.omnibox.showStatusMessage(message, 0);
  }

  private async installSqlPackages(engine: EngineBase) {
    const sqlPackages = await this.sqlPackages();
    for (const pkg of sqlPackages) {
      await engine.registerSqlPackages(pkg);
    }
  }

  private decideTabs(trace: TraceImpl) {
    // Show the list of default tabs, but don't make them active!
    for (const tabUri of trace.tabs.defaultTabs) {
      trace.tabs.showTab(tabUri);
    }
  }
}
