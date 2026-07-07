// Copyright (C) 2026 The Android Open Source Project
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

import m from 'mithril';
import type {ZodRawShape} from 'zod';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import LlmPlugin from '../dev.perfetto.Llm';
import QueryPagePlugin from '../dev.perfetto.QueryPage';
import DataExplorerPlugin from '../dev.perfetto.DataExplorer';
import type {
  ContextProviderRegistration,
  IntellettoToolRegistrar,
  ToolRegistration,
} from './api';
import {ChatPanel} from './chat_panel';
import {ChatSession} from './chat_session';
import {registerDataExplorerTools} from './data_explorer_tools';
import {ContextRegistry, registerCoreContextProviders} from './context';
import {registerCoreTools} from './core_tools';
import {registerTimelineTools} from './timeline_tools';
import {ToolRegistry} from './tools';

const SIDE_PANEL_URI = 'dev.perfetto.Intelletto#Chat';

/**
 * The assistant plugin. Trace-scoped: one instance (and one tool registry) per
 * loaded trace. Implements IntellettoToolRegistrar so dependent plugins can
 * contribute tools via `ctx.plugins.getPlugin(IntellettoPlugin).registerTool`.
 */
export default class IntellettoPlugin
  implements PerfettoPlugin, IntellettoToolRegistrar
{
  static readonly id = 'dev.perfetto.Intelletto';
  static readonly description =
    'Conversational AI assistant. Ask about your trace in natural language; ' +
    'it queries the trace and drives the UI. Requires the dev.perfetto.Llm ' +
    'gateway and an LLM protocol plugin. Other plugins can register their own ' +
    'tools via getPlugin(IntellettoPlugin).registerTool().';
  static readonly dependencies = [
    LlmPlugin,
    QueryPagePlugin,
    DataExplorerPlugin,
  ];

  // The shared tool registry for this trace. Core tools plus any contributed by
  // other plugins land here; the chat panel hands it to the agent.
  private readonly tools = new ToolRegistry();

  // The shared context-provider registry for this trace. Core providers (page,
  // selection) plus any contributed by other plugins; the chat panel samples it
  // for the context strip and the prompt.
  private readonly context = new ContextRegistry();

  // The conversation state for this trace. Owned here - not by the chat panel
  // component - so the conversation survives the side panel being closed and
  // reopened. Created lazily on first panel open, so context providers
  // registered by dependent plugins are in by the time the system prompt is
  // assembled.
  private session?: ChatSession;

  constructor(private readonly trace: Trace) {
    registerCoreTools(this.tools, this.trace);
    registerTimelineTools(this.tools, this.trace);
    registerCoreContextProviders(this.context, this.trace);
  }

  /**
   * IntellettoToolRegistrar: contribute a tool the assistant can call. Call
   * from a dependent plugin's onTraceLoad.
   */
  registerTool<S extends ZodRawShape>(tool: ToolRegistration<S>): void {
    this.tools.registerTool(tool);
  }

  /**
   * IntellettoToolRegistrar: contribute a context provider describing what the
   * user is looking at. Call from a dependent plugin's onTraceLoad.
   */
  registerContextProvider(provider: ContextProviderRegistration): void {
    this.context.registerContextProvider(provider);
  }

  /** Register the chat side panel and the open-assistant command for a trace. */
  async onTraceLoad(trace: Trace): Promise<void> {
    const gateway = LlmPlugin.gateway;

    // One chat session per trace - the conversation is scoped to the open
    // trace and kept in memory only (re-created on the next trace load). It
    // reads the shared registries, so tools registered by other plugins
    // (before or after the panel is first opened) are all visible to the
    // agent.
    trace.sidePanel.registerTab({
      uri: SIDE_PANEL_URI,
      title: 'Intelletto',
      icon: 'psychology',
      render: () => {
        this.session ??= new ChatSession(gateway, this.tools, this.context);
        return m(ChatPanel, {
          gateway,
          session: this.session,
          context: this.context,
        });
      },
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.Intelletto#Open',
      name: 'Intelletto: Open assistant',
      callback: () => trace.sidePanel.showTab(SIDE_PANEL_URI),
      defaultHotkey: 'Mod+Shift+A',
    });

    // Pull in the Data Explorer graph tools and selected-node context.
    registerDataExplorerTools(this.tools, this.context, trace);
  }
}

// Re-export the public API types so dependents can import them from the plugin
// entry point: `import IntellettoPlugin, {ToolRegistration} from '...'`.
export type {
  ContextProviderRegistration,
  ContextSnapshot,
  IntellettoToolRegistrar,
  ToolRegistration,
} from './api';
