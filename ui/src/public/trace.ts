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

import type {z} from 'zod';
import type {TraceInfo} from './trace_info';
import type {Engine} from '../trace_processor/engine';
import type {App} from './app';
import type {TabManager} from './tab';
import type {TrackManager} from './track';
import type {Timeline} from './timeline';
import type {Workspace, WorkspaceManager} from './workspace';
import type {SelectionManager} from './selection';
import type {ScrollToArgs} from './scroll_helper';
import type {NoteManager} from './note';
import type {DisposableStack} from '../base/disposable_stack';
import type {Evt} from '../base/events';
import type {StatusbarManager} from './statusbar';
import type {MinimapManager} from './minimap';
import type {SearchManager} from './search';
import type {InitialPageManager} from './initial_page';

// Lists all the possible event listeners using the key as the event name and
// the type as the type of the callback.
export interface EventListeners {
  traceready: () => Promise<void> | void;
}

/**
 * Describes a persistent storage container backed by the trace's permalink state.
 */
export interface StorageDescriptor<T> {
  // A unique identifier for the storage within the trace's permalink state.
  readonly id: string;
  // The Zod schema used for validating the stored value upon restore.
  readonly schema: z.ZodType<T>;
  // The default value used when no state is present or if validation fails.
  readonly defaultValue: T;
  // Optional human-readable name.
  readonly name?: string;
  // Optional description.
  readonly description?: string;
}

/** @deprecated Use StorageDescriptor instead. */
export type StoreDescriptor<T> = StorageDescriptor<T>;

/**
 * A persistent state container with get and set methods, backed by the trace's
 * permalink state.
 */
export interface Storage<T> {
  // Get the current value, validated by the schema or falling back to default.
  get(): T;
  // Update the stored value.
  set(value: T): void;
  // Reset the stored value back to the default value.
  reset(): void;
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
  readonly engine: Engine;
  readonly notes: NoteManager;
  readonly timeline: Timeline;
  readonly tabs: TabManager;
  readonly tracks: TrackManager;
  readonly selection: SelectionManager;
  readonly currentWorkspace: Workspace;
  readonly defaultWorkspace: Workspace;
  readonly workspaces: WorkspaceManager;
  readonly traceInfo: TraceInfo;
  readonly statusbar: StatusbarManager;
  readonly minimap: MinimapManager;
  readonly search: SearchManager;
  readonly initialPage: InitialPageManager;

  // Events.
  onTraceReady: Evt<void>;

  // Scrolls to the given track and/or time. Does NOT change the current
  // selection.
  scrollTo(args: ScrollToArgs): void;

  /**
   * Register a persistent storage container backed by the trace's permalink state.
   *
   * When loading a permalink, the value will be restored from the permalink if
   * valid according to the schema, or fall back to defaultValue.
   */
  registerStorage<T>(descriptor: StorageDescriptor<T>): Storage<T>;

  // Returns the blob of the current trace file.
  // If the trace is opened from a file or postmessage, the blob is returned
  // immediately. If the trace is opened from URL, this causes a re-download of
  // the trace. It will throw if traceInfo.downloadable === false.
  getTraceFile(): Promise<Blob>;

  // List of errors that were encountered while loading the trace by the TS
  // code. These are on top of traceInfo.importErrors, which is a summary of
  // what TraceProcessor reports on the stats table at import time.
  get loadingErrors(): ReadonlyArray<string>;
  // Trace scoped disposables. Will be destroyed when the trace is unloaded.
  readonly trash: DisposableStack;
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
