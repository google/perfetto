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

import {Migrate, Store} from '../base/store';
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

// Lists all the possible event listeners using the key as the event name and
// the type as the type of the callback.
export interface EventListeners {
  traceready(): Promise<void> | void;
}

/**
 * The main API endpoint for interacting with a loaded trace.
 *
 * This interface extends App and provides trace-specific functionality. Each
 * plugin receives its own scoped instance when a trace is loaded via the
 * onTraceLoad() lifecycle hook. The instance automatically cleans up
 * plugin-specific resources when the trace is closed.
 *
 * Note: There are N+1 instances of this interface - one per plugin plus one
 * for the core UI code.
 */
export interface Trace extends App {
  /**
   * Reference to the parent App instance.
   *
   * Provides access to app-level functionality that persists across trace
   * loads. Use this when you need to access features not specific to the
   * current trace.
   */
  readonly app: App;

  /**
   * The SQL query engine for this trace.
   *
   * Provides access to the underlying TraceProcessor instance, allowing you to
   * execute SQL queries against the trace database, create tables and views,
   * and access trace data. This is the primary interface for querying trace
   * data.
   */
  readonly engine: Engine;

  /**
   * Manages user-created notes and annotations.
   *
   * Notes are markers or spans that users can add to the timeline to annotate
   * interesting events or time ranges. Use this to programmatically create,
   * modify, or remove notes. Notes are persisted in permalinks.
   */
  readonly notes: NoteManager;

  /**
   * Controls the timeline viewport and time-related state.
   *
   * Manages the visible time window, zoom level, time selection span, hover
   * cursor position, timestamp formatting, and highlighted slices. Use this to
   * programmatically navigate the timeline or respond to time-based user
   * interactions.
   */
  readonly timeline: Timeline;

  /**
   * Manages bottom drawer tabs.
   *
   * Tabs appear in the bottom drawer and typically show details, query
   * results, or custom content. Use this to register new tabs (either
   * persistent or ephemeral) and control their visibility. Tabs registered
   * here are automatically cleaned up when the trace closes.
   */
  readonly tabs: TabManager;

  /**
   * Central registry for all tracks in this trace.
   *
   * Tracks are the horizontal lanes that visualize trace data. Use this to
   * register new tracks, retrieve track descriptors, manage track lifecycle,
   * and configure track filtering criteria. Each track must have a unique URI.
   */
  readonly tracks: TrackManager;

  /**
   * Manages the current UI selection state.
   *
   * The selection can be a single event, a time range (area), a track, a note,
   * or other entities. Use this to programmatically change what's selected,
   * query the current selection, or register custom selection tabs. Selection
   * state is persisted in permalinks.
   */
  readonly selection: SelectionManager;

  /**
   * The currently active workspace.
   *
   * A workspace is a specific arrangement of tracks and groups. This property
   * always points to whichever workspace the user is currently viewing. When
   * users switch workspaces, this reference updates automatically.
   */
  readonly currentWorkspace: Workspace;

  /**
   * The default workspace containing all tracks.
   *
   * This is the primary workspace that contains the full track hierarchy as
   * initially loaded. Other workspaces may contain subsets or alternative
   * arrangements of these tracks. This workspace is never deleted.
   */
  readonly defaultWorkspace: Workspace;

  /**
   * Manages multiple workspaces.
   *
   * Workspaces allow users to create different views of the trace with
   * different track arrangements. Use this to create new workspaces, switch
   * between them, or access the list of all workspaces. Each workspace
   * maintains its own track hierarchy.
   */
  readonly workspaces: WorkspaceManager;

  /**
   * Immutable metadata about the loaded trace.
   *
   * Contains information such as trace title, start and end timestamps, trace
   * type, source, timezone offset, whether it's downloadable, and any import
   * errors reported by TraceProcessor. This data is read-only and set when the
   * trace is loaded.
   */
  readonly traceInfo: TraceInfo;

  /**
   * Manages status bar items.
   *
   * The status bar appears at the bottom of the UI and displays contextual
   * information. Use this to register custom status bar items that show
   * plugin-specific information or metrics.
   */
  readonly statusbar: StatusbarManager;

  /**
   * Manages minimap visualization providers.
   *
   * The minimap is the small overview visualization at the top of the timeline
   * that shows the entire trace at a glance. Use this to register custom data
   * providers that contribute to the minimap visualization.
   */
  readonly minimap: MinimapManager;

  /**
   * Manages the global search functionality.
   *
   * Provides search capabilities across all tracks. Use this to
   * programmatically trigger searches, access search results, or navigate
   * between matches.
   */
  readonly search: SearchManager;

  /**
   * Event that fires when the trace is fully loaded and ready.
   *
   * This event fires after all plugins' onTraceLoad() hooks have completed and
   * the trace is fully initialized. Use this to perform actions that require
   * the complete trace state, such as selecting default items or triggering
   * initial queries.
   */
  readonly onTraceReady: Evt<void>;

  /**
   * Errors encountered during trace loading.
   *
   * Contains TypeScript-level errors that occurred while loading and
   * processing the trace. This is separate from traceInfo.importErrors, which
   * contains errors reported by the TraceProcessor during trace import.
   */
  readonly loadingErrors: ReadonlyArray<string>;

  /**
   * Cleanup stack for trace-scoped resources.
   *
   * Resources added to this stack are automatically disposed when the trace is
   * closed. Use this to register cleanup callbacks for resources that should
   * be released when the trace unloads (e.g., event listeners, timers,
   * database connections).
   */
  readonly trash: DisposableStack;

  /**
   * Scrolls the timeline to a specific track and/or time.
   *
   * This method navigates the viewport without changing the current selection.
   * You can scroll to a track (optionally expanding its parent groups), a time
   * range, or both. Useful for implementing "jump to" functionality.
   *
   * @param args - Object specifying the scroll target (track URI and/or time
   *   range)
   */
  scrollTo(args: ScrollToArgs): void;

  /**
   * Retrieves the trace file as a Blob.
   *
   * For traces loaded from files or received via postMessage, returns the blob
   * immediately. For traces loaded from URLs, triggers a re-download. This is
   * useful for implementing export, sharing, or conversion features.
   *
   * @returns A promise that resolves to the trace file blob
   * @throws Error if traceInfo.downloadable is false
   */
  getTraceFile(): Promise<Blob>;

  /**
   * Creates a trace-scoped persistent store.
   *
   * The store's contents are automatically serialized into permalinks, allowing
   * plugin-specific state to be preserved across sessions. Each store is
   * identified by a unique ID and must provide a migration function to handle
   * schema evolution.
   *
   * @param id - Unique identifier for this store (should be namespaced to your
   *   plugin)
   * @param migrate - Function to migrate existing state to the current schema.
   *   Receives the stored value (or undefined) and should return a valid T
   * @returns A Store instance for reading and writing state
   */
  mountStore<T>(id: string, migrate: Migrate<T>): Store<T>;
}

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
  trace: Trace;
}
