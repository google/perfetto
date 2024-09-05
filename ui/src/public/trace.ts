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
import {time, TimeSpan} from '../base/time';
import {TraceContext} from '../frontend/trace_context';
import {Engine} from '../trace_processor/engine';
import {App} from './app';
import {PromptOption} from './omnibox';
import {TabDescriptor} from './tab';
import {TrackDescriptor} from './track';
import {LegacyDetailsPanel} from './details_panel';
import {Workspace} from './workspace';

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

  // Control over the main timeline.
  timeline: {
    // Bring a timestamp into view.
    panToTimestamp(ts: time): void;

    // Move the viewport
    setViewportTime(start: time, end: time): void;

    // A span representing the current viewport location
    readonly viewport: TimeSpan;

    // Access the default workspace - used for adding, removing and reorganizing
    // tracks
    readonly workspace: Workspace;
  };

  // Control over the bottom details pane.
  tabs: {
    // Creates a new tab running the provided query.
    openQuery(query: string, title: string): void;

    // Add a tab to the tab bar (if not already) and focus it.
    showTab(uri: string): void;

    // Remove a tab from the tab bar.
    hideTab(uri: string): void;
  };

  // Register a new track against a unique key known as a URI. The track is not
  // shown by default and callers need to either manually add it to a
  // Workspace or use registerTrackAndShowOnTraceLoad() below.
  registerTrack(trackDesc: TrackDescriptor): void;

  // Register a new tab for this plugin. Will be unregistered when the plugin
  // is deactivated or when the trace is unloaded.
  registerTab(tab: TabDescriptor): void;

  // Suggest that a tab should be shown immediately.
  addDefaultTab(uri: string): void;

  // Register a hook into the current selection tab rendering logic that allows
  // customization of the current selection tab content.
  registerDetailsPanel(sel: LegacyDetailsPanel): void;

  // Create a store mounted over the top of this plugin's persistent state.
  mountStore<T>(migrate: Migrate<T>): Store<T>;

  readonly trace: TraceContext;

  // When the trace is opened via postMessage deep-linking, returns the sub-set
  // of postMessageData.pluginArgs[pluginId] for the current plugin. If not
  // present returns undefined.
  readonly openerPluginArgs?: {[key: string]: unknown};

  prompt(text: string, options?: PromptOption[]): Promise<string>;
}
