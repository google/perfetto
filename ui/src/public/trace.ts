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

import {TraceInfo} from './trace_info';
import {Engine} from '../trace_processor/engine';
import {App} from './app';
import {TabManager} from './tab';
import {TrackManager} from './track';
import {Timeline} from './timeline';
import {Workspace, WorkspaceManager} from './workspace';
import {SelectionManager} from './selection';
import {ScrollToArgs} from './scroll_helper';
import {NoteManager} from './note';
import {DisposableStack} from '../base/disposable_stack';
import {Evt} from '../base/events';
import {StatusbarManager} from './statusbar';
import {MinimapManager} from './minimap';
import {SearchManager} from './search';
import {Migrate, Store} from '../base/store';

// Lists all the possible event listeners using the key as the event name and
// the type as the type of the callback.
export interface EventListeners {
  traceready: () => Promise<void> | void;
}

/**
 * The main API endpoint to interact programmatically with the UI and alter its
 * state once a trace is loaded. There are N+1 instances of this interface,
 * one for each plugin and one for the core (which, however, gets to see the
 * full AppImpl behind this to access all the internal methods).
 * This interface is passed to plugins' onTraceLoad() hook and is injected
 * pretty much everywhere in core.
 */
export interface Trace extends App {
  readonly app: App;

  /**
   * The engine associated with this trace. This engine is used to access the
   * TraceProcessor instance for this trace, in order to run queries and obtain
   * other trace relevant data.
   */
  readonly engine: Engine;

  /**
   * Stores notes and annotations added by the user to this trace.
   */
  readonly notes: NoteManager;

  /**
   * Controls the timeline and viewport for this trace.
   */
  readonly timeline: Timeline;

  /**
   * Stores and provides access to tabs for this trace.
   */
  readonly tabs: TabManager;

  /**
   * Stores and provides access to tracks for this trace.
   */
  readonly tracks: TrackManager;

  /**
   * Manages the current selection for this trace.
   */
  readonly selection: SelectionManager;

  /**
   * A pointer to the current track workspace for this trace.
   */
  readonly currentWorkspace: Workspace;

  /**
   * A reference to the default track workspace for this trace.
   */
  readonly defaultWorkspace: Workspace;

  /**
   * Stores and provides access to workspaces for this trace.
   */
  readonly workspaces: WorkspaceManager;

  /**
   * Readonly metadata about the trace.
   */
  readonly traceInfo: TraceInfo;

  /**
   * Manages the status bar for this trace.
   */
  readonly statusbar: StatusbarManager;

  /**
   * Manages minimap renderers for this trace.
   */
  readonly minimap: MinimapManager;

  /**
   * Manages the search functionality for this trace.
   */
  readonly search: SearchManager;

  // An event that fires when the trace is fully loaded and ready, after all the
  // onTraceLoad() hooks have run.
  readonly onTraceReady: Evt<void>;

  // List of errors that were encountered while loading the trace by the TS
  // code. These are on top of traceInfo.importErrors, which is a summary of
  // what TraceProcessor reports on the stats table at import time.
  readonly loadingErrors: ReadonlyArray<string>;

  // Trace scoped disposables. Will be destroyed when the trace is unloaded.
  readonly trash: DisposableStack;

  // Scrolls to the given track and/or time. Does NOT change the current
  // selection.
  scrollTo(args: ScrollToArgs): void;

  // Returns the blob of the current trace file.
  // If the trace is opened from a file or postmessage, the blob is returned
  // immediately. If the trace is opened from URL, this causes a re-download of
  // the trace. It will throw if traceInfo.downloadable === false.
  getTraceFile(): Promise<Blob>;

  /**
   * Mount a mutable trace scoped store, the contents of which is persisted into
   * permalinks.
   *
   * @param id The unique ID of the store.
   * @param migrate A migration function that will be used to migrate whatever
   * existing state is present into the desired type T.
   */
  mountStore<T>(id: string, migrate: Migrate<T>): Store<T>;
}

//   // When the trace is opened via postMessage deep-linking, returns the sub-set
// // of postMessageData.pluginArgs[pluginId] for the current plugin. If not
// // present returns undefined.
// readonly openerPluginArgs?: {[key: string]: unknown};

/**
 * A convenience interface to inject the App in Mithril components.
 * Example usage:
 *
 * class MyComponent implements m.ClassComponent<TraceAttrs> {
 *   oncreate({attrs}: m.CVnodeDOM<AppAttrs>): void {
 *     attrs.trace.engine.runQuery(...);
 *   }
 * }
 */
export interface TraceAttrs {
  readonly trace: Trace;
}
