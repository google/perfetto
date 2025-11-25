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
import {Engine, EngineBase} from '../trace_processor/engine';
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
import {SettingDescriptor} from '../public/settings';
import {SettingsManagerImpl} from './settings_manager';
import {MinimapManagerImpl} from './minimap_manager';
import {TraceStream} from '../public/stream';

/**
 * This implementation provides the plugin access to trace related resources,
 * such as the engine and the store. This exists for the whole duration a plugin
 * is active AND a trace is loaded.
 * There are N+1 instances of this for each trace, one for each plugin plus one
 * for the core.
 */
export class TraceImpl implements Trace, Disposable {
  readonly engine: Engine;
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
      this.timeline,
      this.workspaces,
      this.tracks,
    );

    this.selection = new SelectionManagerImpl(
      this.engine,
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

    // CommandManager is global. Here we intercept the registerCommand() because
    // we want any commands registered via the Trace interface to be
    // unregistered when the trace unloads (before a new trace is loaded) to
    // avoid ending up with duplicate commands.
    this.commandMgrProxy = createProxy(app.commands, {
      registerCommand: (cmd: Command) => {
        const disposable = app.commands.registerCommand(cmd);
        this.trash.use(disposable);
        return disposable;
      },
    });

    // Likewise, remove all trace-scoped sidebar entries when the trace unloads.
    this.sidebarProxy = createProxy(app.sidebar, {
      addMenuItem: (menuItem: SidebarMenuItem) => {
        const disposable = app.sidebar.addMenuItem(menuItem);
        this.trash.use(disposable);
        return disposable;
      },
    });

    this.pageMgrProxy = createProxy(app.pages, {
      registerPage: (pageHandler: PageHandler) => {
        const disposable = app.pages.registerPage(pageHandler);
        this.trash.use(disposable);
        return disposable;
      },
    });

    this.settingsProxy = createProxy(app.settings, {
      register: <T>(setting: SettingDescriptor<T>) => {
        const disposable = app.settings.register(setting);
        this.trash.use(disposable);
        return disposable;
      },
    });
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
