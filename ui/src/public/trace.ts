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
import {LegacyDetailsPanel} from './details_panel';
import {SelectionManager} from './selection';
import {ScrollToArgs} from './scroll_helper';

/**
 * The main API endpoint to interact programmaticaly with the UI and alter its
 * state once a trace is loaded. There are N+1 instances of this interface,
 * one for each plugin and one for the core (which, however, gets to see the
 * full AppImpl behind this to acces all the internal methods).
 * This interface is passed to plugins' onTraceLoad() hook and is injected
 * pretty much everywhere in core.
 */
export interface Trace extends App {
  readonly engine: Engine;
  readonly timeline: Timeline;
  readonly tabs: TabManager;
  readonly tracks: TrackManager;
  readonly selection: SelectionManager;
  readonly workspace: Workspace;
  readonly workspaces: WorkspaceManager;
  readonly traceInfo: TraceInfo;

  // Scrolls to the given track and/or time. Does NOT change the current
  // selection.
  scrollTo(args: ScrollToArgs): void;

  // TODO(primiano): remove this once the Legacy vs non-Legacy details panel is
  // gone. This method is particularly problematic because the method called
  // registerDetailsPanel in TabManagerImpl takes a non-Legacy DetailsPanel, but
  // all plugins use a Legacy one. Keeping this as a bridge for now.
  registerDetailsPanel(detailsPanel: LegacyDetailsPanel): void;

  // Creates and shows a tab with a tabular result for the given query.
  // TODO(primiano): I am not convinced this belongs here, this should probably
  // go under public/lib/query_result_tab.ts as a utility function that takes
  // Trace as an argument. However I need first to plumb App everywhere.
  addQueryResultsTab(query: string, title: string): void;

  // Create a store mounted over the top of this plugin's persistent state.
  mountStore<T>(migrate: Migrate<T>): Store<T>;

  // When the trace is opened via postMessage deep-linking, returns the sub-set
  // of postMessageData.pluginArgs[pluginId] for the current plugin. If not
  // present returns undefined.
  readonly openerPluginArgs?: {[key: string]: unknown};
}
