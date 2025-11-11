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
import {Command} from '../public/command';
import {Trace} from '../public/trace';
import {ScrollToArgs} from '../public/scroll_helper';
import {EngineBase} from '../trace_processor/engine';
import {CommandManagerImpl, parseUrlCommands} from './command_manager';
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
import {FlowManager} from './flow_manager';
import {AppImpl, OpenTraceArrayBufArgs} from './app_impl';
import {PluginManagerImpl} from './plugin_manager';
import {RouteArgs} from '../public/route_schema';
import {Analytics} from '../public/analytics';
import {fetchWithProgress} from '../base/http_utils';
import {TraceInfoImpl} from './trace_info_impl';
import {PageHandler, PageManager} from '../public/page';
import {createProxy} from '../base/utils';
import {PageManagerImpl} from './page_manager';
import {FeatureFlagManager, FlagSettings} from '../public/feature_flag';
import {SerializedAppState} from './state_serialization_schema';
import {featureFlags} from './feature_flags';
import {EvtSource} from '../base/events';
import {Raf} from '../public/raf';
import {StatusbarManagerImpl} from './statusbar_manager';
import {Setting, SettingDescriptor} from '../public/settings';
import {SettingsManagerImpl} from './settings_manager';
import {MinimapManagerImpl} from './minimap_manager';
import {isStartupCommandAllowed} from './startup_command_allowlist';
import {TraceStream} from '../public/stream';

/**
 * This implementation provides the plugin access to trace related resources,
 * such as the engine and the store. This exists for the whole duration a plugin
 * is active AND a trace is loaded.
 * There are N+1 instances of this for each trace, one for each plugin plus one
 * for the core.
 */
export class TraceImpl implements Trace, Disposable {
  readonly engine: EngineBase;
  readonly search: SearchManagerImpl;
  readonly selection: SelectionManagerImpl;
  readonly tabs = new TabManagerImpl();
  readonly timeline: TimelineImpl;
  readonly traceInfo: TraceInfoImpl;
  readonly tracks = new TrackManagerImpl();
  readonly workspaces = new WorkspaceManagerImpl();
  readonly notes = new NoteManagerImpl();
  readonly flows: FlowManager;
  readonly scrollHelper: ScrollHelper;
  readonly trash = new DisposableStack();
  readonly onTraceReady = new EvtSource<void>();
  readonly statusbar = new StatusbarManagerImpl();
  readonly minimap = new MinimapManagerImpl();
  readonly loadingErrors: string[] = [];
  readonly app: AppImpl;
  readonly store = createStore<Record<string, unknown>>({});

  // Do we need this?
  readonly pluginSerializableState = createStore<{[key: string]: {}}>({});

  constructor(app: AppImpl, engine: EngineBase, traceInfo: TraceInfoImpl) {
    this.app = app;
    this.engine = engine;
    this.trash.use(engine);
    this.traceInfo = traceInfo;

    this.timeline = new TimelineImpl(
      traceInfo,
      app.timestampFormat,
      app.durationPrecision,
      app.timezoneOverride,
    );

    this.scrollHelper = new ScrollHelper(
      this.traceInfo,
      this.timeline,
      this.workspaces,
      this.tracks,
    );

    this.selection = new SelectionManagerImpl(
      this.engine,
      this.timeline,
      this.tracks,
      this.notes,
      this.scrollHelper,
      this.onSelectionChange.bind(this),
    );

    this.notes.onNoteDeleted = (noteId) => {
      if (
        this.selection.selection.kind === 'note' &&
        this.selection.selection.id === noteId
      ) {
        this.selection.clearSelection();
      }
    };

    this.flows = new FlowManager(
      engine.getProxy('FlowManager'),
      this.tracks,
      this.selection,
    );

    this.search = new SearchManagerImpl({
      timeline: this.timeline,
      trackManager: this.tracks,
      engine: this.engine,
      workspace: this.workspaces.currentWorkspace,
      onResultStep: this.onResultStep.bind(this),
    });

    // CRITICAL ORDER: URL commands MUST execute before settings commands!
    // This ordering has subtle but important implications:
    // - URL commands are trace-specific and should establish initial state
    // - Settings commands are user preferences that should override URL defaults
    // - Changing this order could break trace sharing and user customization
    // DO NOT REORDER without understanding the full impact!
    const urlCommands =
      parseUrlCommands(app.initialRouteArgs.startupCommands) ?? [];
    const settingsCommands = app.startupCommandsSetting.get();

    // Combine URL and settings commands - runtime allowlist checking will handle filtering
    const allStartupCommands = [...urlCommands, ...settingsCommands];
    const enforceAllowlist = app.enforceStartupCommandAllowlistSetting.get();

    const traceUnloadTrash = this.trash;
    // CommandManager is global. Here we intercept the registerCommand() because
    // we want any commands registered via the Trace interface to be
    // unregistered when the trace unloads (before a new trace is loaded) to
    // avoid ending up with duplicate commands.
    this.commandMgrProxy = createProxy(app.commands, {
      registerCommand(cmd: Command): Disposable {
        const disposable = app.commands.registerCommand(cmd);
        traceUnloadTrash.use(disposable);
        return disposable;
      },

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
          app.commands.setAllowlistCheck(isStartupCommandAllowed);
        }

        try {
          for (const command of allStartupCommands) {
            try {
              // Execute through proxy to access both global and trace-specific
              // commands.
              await app.commands.runCommand(command.id, ...command.args);
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
          app.commands.setAllowlistCheck(() => true);
        }
      },
    });

    // Likewise, remove all trace-scoped sidebar entries when the trace unloads.
    this.sidebarProxy = createProxy(app.sidebar, {
      addMenuItem(menuItem: SidebarMenuItem): Disposable {
        const disposable = app.sidebar.addMenuItem(menuItem);
        traceUnloadTrash.use(disposable);
        return disposable;
      },
    });

    this.pageMgrProxy = createProxy(app.pages, {
      registerPage(pageHandler: PageHandler): Disposable {
        const disposable = app.pages.registerPage(pageHandler);
        traceUnloadTrash.use(disposable);
        return disposable;
      },
    });

    this.settingsProxy = createProxy(app.settings, {
      register<T>(setting: SettingDescriptor<T>): Setting<T> {
        const disposable = app.settings.register(setting);
        traceUnloadTrash.use(disposable);
        return disposable;
      },
    });

    // TODO in plugin manager - inject pluginid into:
    // - pages
    // - tracks

    // // Intercept the registerTrack() method to inject the pluginId into tracks.
    // this.trackMgrProxy = createProxy(ctx.trackMgr, {
    //   registerTrack(trackDesc: Track): Disposable {
    //     return ctx.trackMgr.registerTrack({...trackDesc, pluginId});
    //   },
    // });

    // this.pageMgrProxy = createProxy(ctx.appCtx.pageMgr, {
    //   registerPage(pageHandler: PageHandler): Disposable {
    //     const disposable = appImpl.pages.registerPage({
    //       ...pageHandler,
    //       pluginId: appImpl.pluginId,
    //     });
    //     traceUnloadTrash.use(disposable);
    //     return disposable;
    //   },
    // });
  }

  // This method wires up changes to selection to side effects on search and
  // tabs. This is to avoid entangling too many dependencies between managers.
  private onSelectionChange(selection: Selection, opts: SelectionOpts) {
    const {clearSearch = true, switchToCurrentSelectionTab = true} = opts;
    if (clearSearch) {
      this.search.reset();
    }
    if (switchToCurrentSelectionTab && selection.kind !== 'empty') {
      this.tabs.showCurrentSelectionTab();
    }

    this.flows.updateFlows(selection);
  }

  private onResultStep(searchResult: SearchResult) {
    this.selection.selectSearchResult(searchResult);
  }

  [Symbol.dispose]() {
    this.trash.dispose();
  }

  private readonly commandMgrProxy: CommandManagerImpl;
  private readonly sidebarProxy: SidebarManagerImpl;
  private readonly pageMgrProxy: PageManagerImpl;
  private readonly settingsProxy: SettingsManagerImpl;

  scrollTo(where: ScrollToArgs): void {
    this.scrollHelper.scrollTo(where);
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

  get trace() {
    return this;
  }

  get currentWorkspace() {
    return this.workspaces.currentWorkspace;
  }

  get defaultWorkspace() {
    return this.workspaces.defaultWorkspace;
  }

  get commands(): CommandManagerImpl {
    return this.commandMgrProxy;
  }

  get sidebar(): SidebarManagerImpl {
    return this.sidebarProxy;
  }

  get pages(): PageManager {
    return this.pageMgrProxy;
  }

  get omnibox(): OmniboxManagerImpl {
    return this.app.omnibox;
  }

  get plugins(): PluginManagerImpl {
    return this.app.plugins;
  }

  get analytics(): Analytics {
    return this.app.analytics;
  }

  get initialRouteArgs(): RouteArgs {
    return this.app.initialRouteArgs;
  }

  get featureFlags(): FeatureFlagManager {
    return {
      register: (settings: FlagSettings) => featureFlags.register(settings),
    };
  }

  get raf(): Raf {
    return this.app.raf;
  }

  navigate(newHash: string): void {
    this.app.navigate(newHash);
  }

  openTraceFromFile(file: File) {
    return this.app.openTraceFromFile(file);
  }

  openTraceFromUrl(url: string, serializedAppState?: SerializedAppState) {
    return this.app.openTraceFromUrl(url, serializedAppState);
  }

  openTraceFromStream(stream: TraceStream) {
    return this.app.openTraceFromStream(stream);
  }

  openTraceFromBuffer(
    args: OpenTraceArrayBufArgs,
    serializedAppState?: SerializedAppState,
  ) {
    return this.app.openTraceFromBuffer(args, serializedAppState);
  }

  closeCurrentTrace(): void {
    this.app.closeCurrentTrace();
  }

  get settings(): SettingsManagerImpl {
    return this.settingsProxy;
  }

  get isInternalUser(): boolean {
    return this.app.isInternalUser;
  }

  get perfDebugging() {
    return this.app.perfDebugging;
  }

  mountStore<T>(id: string, migrate: Migrate<T>): Store<T> {
    return this.store.createSubStore([id], migrate);
  }
}

// A convenience interface to inject the App in Mithril components.
export interface TraceImplAttrs {
  trace: TraceImpl;
}

export interface OptionalTraceImplAttrs {
  trace?: TraceImpl;
}
