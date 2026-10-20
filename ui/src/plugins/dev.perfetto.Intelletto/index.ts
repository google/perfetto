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

import {z} from 'zod';
import m from 'mithril';
import type {App} from '../../public/app';
import type {Setting} from '../../public/settings';
import type {ZodRawShape} from 'zod';
import type {PerfettoPlugin, PluginStatus} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import LlmPlugin from '../dev.perfetto.Llm';
import type {ModelPath} from '../dev.perfetto.Llm/gateway';
import QueryPagePlugin from '../dev.perfetto.QueryPage';
import DataExplorerPlugin from '../dev.perfetto.DataExplorer';
import type {
  ContextProviderRegistration,
  IntellettoToolRegistrar,
  ToolRegistration,
} from './api';
import {ChatPanel} from './chat_panel';
import {ChatSession} from './chat_session';
import {ConversationStore} from './conversation_store';
import {registerDataExplorerTools} from './data_explorer_tools';
import {
  ContextRegistry,
  registerAppContextProviders,
  registerCoreContextProviders,
} from './context';
import {registerCoreTools} from './core_tools';
import {registerTimelineTools} from './timeline_tools';
import {ToolRegistry} from './tools';
import {registerWebMCPTools} from './webmcp';

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
  static readonly status: PluginStatus = 'experimental';
  static readonly dependencies = [
    LlmPlugin,
    QueryPagePlugin,
    DataExplorerPlugin,
  ];

  // App-scoped persisted setting: the user's last-selected model for the
  // assistant. Headless (hidden from settings page) — changed via the sidebar
  // dropdown. Survives page reloads and is shared across all traces.
  private static selectedModelSetting: Setting<ModelPath | undefined>;

  // App-scoped active chat session, shared across traces so opening or
  // switching traces keeps the panel and conversation alive.
  private static session?: ChatSession;

  // The IntellettoPlugin instance for the currently loaded trace, if any.
  // Supplies trace-scoped tools and context providers.
  private static activeInstance?: IntellettoPlugin;

  // Fallback registries used when no trace is loaded.
  private static readonly emptyTools = new ToolRegistry();
  private static appContext = new ContextRegistry();

  private static getActiveTools(): ToolRegistry {
    return (
      IntellettoPlugin.activeInstance?.tools ?? IntellettoPlugin.emptyTools
    );
  }

  private static getActiveContext(): ContextRegistry {
    return (
      IntellettoPlugin.activeInstance?.context ?? IntellettoPlugin.appContext
    );
  }

  static onActivate(app: App): void {
    IntellettoPlugin.selectedModelSetting = app.settings.register({
      id: `${IntellettoPlugin.id}#SelectedModel`,
      name: 'Selected model',
      description:
        'The LLM model selected for the Intelletto assistant. Persisted ' +
        'across traces and page reloads.',
      schema: z
        .object({
          providerId: z.string(),
          modelId: z.string(),
        })
        .or(z.undefined()),
      defaultValue: undefined,
      headless: true,
    });
    IntellettoPlugin.conversationStore = new ConversationStore();
    IntellettoPlugin.appContext = new ContextRegistry();
    registerAppContextProviders(IntellettoPlugin.appContext, app);

    // App-scoped chat side panel so Intelletto stays available before a trace
    // is loaded and persists across trace switches. Tools and context are
    // resolved dynamically from the active trace's plugin instance.
    app.sidePanel.registerTab({
      uri: SIDE_PANEL_URI,
      title: 'Intelletto',
      icon: 'psychology',
      render: () => {
        const gateway = LlmPlugin.gateway;
        const store = IntellettoPlugin.conversationStore;
        const selectedModelSetting = IntellettoPlugin.selectedModelSetting;
        const getTools = () => IntellettoPlugin.getActiveTools();
        const getContext = () => IntellettoPlugin.getActiveContext();
        IntellettoPlugin.session ??= new ChatSession(
          gateway,
          getTools,
          getContext,
          store,
          {model: selectedModelSetting.get()},
        );
        return m(ChatPanel, {
          gateway,
          session: IntellettoPlugin.session,
          context: getContext(),
          selectedModelSetting,
          conversations: store.list(),
          onNewSession: () => {
            if (IntellettoPlugin.session?.loading) {
              IntellettoPlugin.session.cancel();
            }
            IntellettoPlugin.session = undefined;
          },
          onSelectConversation: (id: string) => {
            const stored = store.get(id);
            if (stored === undefined) return;
            if (IntellettoPlugin.session?.loading) {
              IntellettoPlugin.session.cancel();
            }
            IntellettoPlugin.session = new ChatSession(
              gateway,
              getTools,
              getContext,
              store,
              {stored},
            );
          },
          onDeleteConversation: (id: string) => {
            store.delete(id);
            // Deleting the open conversation drops back to a fresh one.
            if (IntellettoPlugin.session?.id === id) {
              IntellettoPlugin.session = undefined;
            }
          },
        });
      },
    });

    app.commands.registerCommand({
      id: 'dev.perfetto.Intelletto#Open',
      name: 'Intelletto: Open assistant',
      callback: () => app.sidePanel.showTab(SIDE_PANEL_URI),
      defaultHotkey: 'Mod+Shift+A',
    });
  }

  // App-scoped store of past conversations, persisted to localStorage and
  // shared across all traces.
  private static conversationStore: ConversationStore;

  // The shared tool registry for this trace. Core tools plus any contributed by
  // other plugins land here; the chat panel hands it to the agent.
  private readonly tools = new ToolRegistry();

  // The shared context-provider registry for this trace. Core providers (trace,
  // page, viewport, selection) plus any contributed by other plugins; the chat
  // panel samples it for the context strip and the prompt.
  private readonly context = new ContextRegistry();

  constructor(private readonly trace: Trace) {
    registerCoreTools(this.tools, this.trace);
    registerTimelineTools(this.tools, this.trace);
    registerCoreContextProviders(this.context, this.trace);
    if (
      IntellettoPlugin.activeInstance !== undefined &&
      IntellettoPlugin.session?.loading
    ) {
      IntellettoPlugin.session.cancel();
    }
    IntellettoPlugin.activeInstance = this;
    trace.trash.defer(() => {
      if (IntellettoPlugin.activeInstance === this) {
        if (IntellettoPlugin.session?.loading) {
          IntellettoPlugin.session.cancel();
        }
        IntellettoPlugin.activeInstance = undefined;
      }
    });
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

  /** Register trace-scoped tools (Data Explorer, WebMCP) for a loaded trace. */
  async onTraceLoad(trace: Trace): Promise<void> {
    // Pull in the Data Explorer graph tools and selected-node context.
    registerDataExplorerTools(this.tools, this.context, trace);

    // Register tools with Chrome's experimental WebMCP API so external agents
    // (e.g. Chrome's built-in AI agent, MCP inspector extensions) can discover
    // and call them. No-ops gracefully when the API is not available.
    const webmcpController = registerWebMCPTools(this.tools);
    trace.trash.defer(() => webmcpController?.abort());
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
