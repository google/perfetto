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
import type {SkillInfo} from '../../public/skills';
import type {ZodRawShape} from 'zod';
import type {PerfettoPlugin, PluginStatus} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {Time} from '../../base/time';
import LlmPlugin, {LLM_PROVIDER_SETTING_ID} from '../dev.perfetto.Llm';
import type {
  LlmGateway,
  ModelDetails,
  ModelPath,
} from '../dev.perfetto.Llm/gateway';
import QueryPagePlugin from '../dev.perfetto.QueryPage';
import DataExplorerPlugin from '../dev.perfetto.DataExplorer';
import RecordTraceV2Plugin from '../dev.perfetto.RecordTraceV2';
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
import {type Evidence, EvidenceStore} from './evidence';
import {registerRecordingTools} from './recording_tools';
import {registerSkillTools, skillsPromptSection} from './skill_tools';
import {SYSTEM_PROMPT} from './system_prompt';
import {registerTimelineTools} from './timeline_tools';
import {ToolRegistry} from './tools';
import {registerWebMCPTools} from './webmcp';

const SIDE_PANEL_URI = 'dev.perfetto.Intelletto#Chat';
const EMPTY_SET: ReadonlySet<string> = new Set();

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
    RecordTraceV2Plugin,
  ];

  // App-scoped persisted setting: the user's last-selected model for the
  // assistant. Headless (hidden from settings page) — changed via the sidebar
  // dropdown. Survives page reloads and is shared across all traces.
  private static selectedModelSetting: Setting<ModelPath | undefined>;
  private static skills?: ReadonlyArray<SkillInfo>;
  private static skillsPromise: Promise<ReadonlyArray<SkillInfo>> =
    Promise.resolve([]);

  // App-scoped active chat session, shared across traces so opening or
  // switching traces keeps the panel and conversation alive.
  private static session?: ChatSession;

  // The IntellettoPlugin instance for the currently loaded trace, if any.
  // Supplies trace-scoped tools and context providers.
  private static activeInstance?: IntellettoPlugin;

  // App-scoped registries available even when no trace is loaded (trace-scoped
  // tool registries inherit from appTools so app tools stay available once a
  // trace is open too).
  private static appTools = new ToolRegistry();
  private static appContext = new ContextRegistry();

  private static getActiveTools(): ToolRegistry {
    return IntellettoPlugin.activeInstance?.tools ?? IntellettoPlugin.appTools;
  }

  private static getActiveContext(): ContextRegistry {
    return (
      IntellettoPlugin.activeInstance?.context ?? IntellettoPlugin.appContext
    );
  }

  private static getOrCreateSession(
    gateway: LlmGateway,
    skills: ReadonlyArray<SkillInfo>,
  ): ChatSession {
    return (IntellettoPlugin.session ??= new ChatSession(
      gateway,
      () => IntellettoPlugin.getActiveTools(),
      () => IntellettoPlugin.getActiveContext(),
      () => assembleSystemPrompt(IntellettoPlugin.getActiveContext(), skills),
      IntellettoPlugin.conversationStore,
      {model: IntellettoPlugin.selectedModelSetting.get()},
    ));
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
    // Kick off skill index loading immediately so it's ready by the time the
    // user opens the panel, and redraw once loaded so isLoadingSkills clears.
    IntellettoPlugin.skills = undefined;
    IntellettoPlugin.skillsPromise = app.skills
      .list()
      .then(
        (list) => {
          IntellettoPlugin.skills = list;
          return list;
        },
        () => {
          IntellettoPlugin.skills = [];
          return [];
        },
      )
      .finally(() => m.redraw());

    IntellettoPlugin.conversationStore = new ConversationStore(() =>
      m.redraw(),
    );
    IntellettoPlugin.appTools = new ToolRegistry();
    registerRecordingTools(IntellettoPlugin.appTools, app);
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
        const skills = IntellettoPlugin.skills;
        const session =
          skills !== undefined
            ? IntellettoPlugin.getOrCreateSession(gateway, skills)
            : undefined;
        const models = gateway
          .listModels()
          .filter((rm) => rm.model.roles.includes('agentic'));
        const selectedModel = getValidModel(
          session?.model ?? IntellettoPlugin.selectedModelSetting.get(),
          models,
        );
        const storedConversations = store.list();
        const isStored =
          session !== undefined &&
          storedConversations.some((c) => c.id === session.id);
        const storedSummaries = storedConversations.map((c) => ({
          id: c.id,
          title: c.title,
          updatedAt: c.updatedAt,
          stored: true,
        }));
        const conversations =
          session !== undefined && !isStored
            ? [
                {id: session.id, title: session.title, stored: false},
                ...storedSummaries,
              ]
            : storedSummaries;
        return m(ChatPanel, {
          isAgentRunning: session?.loading ?? false,
          isLoadingSkills: skills === undefined,
          totalTokens: session?.totalTokens,
          onCancel: () => session?.cancel(),
          onRetry:
            session?.canRetry === true && selectedModel !== undefined
              ? () => {
                  void session.retry(selectedModel);
                }
              : undefined,
          onEvidence: (ev) => IntellettoPlugin.activeInstance?.openEvidence(ev),
          conversation: session?.lines ?? [],
          onSendMessage: (msg) => {
            if (selectedModel === undefined) return;
            if (session !== undefined) {
              void session.send(msg, selectedModel);
            } else {
              void IntellettoPlugin.skillsPromise.then((loadedSkills) => {
                void IntellettoPlugin.getOrCreateSession(
                  gateway,
                  loadedSkills,
                ).send(msg, selectedModel);
              });
            }
          },
          conversations,
          activeConversationId: session?.id,
          onOpenConversation: (id) => {
            const stored = store.get(id);
            if (stored === undefined || skills === undefined) return;
            if (IntellettoPlugin.session?.loading) {
              IntellettoPlugin.session.cancel();
            }
            IntellettoPlugin.session = new ChatSession(
              gateway,
              () => IntellettoPlugin.getActiveTools(),
              () => IntellettoPlugin.getActiveContext(),
              () =>
                assembleSystemPrompt(
                  IntellettoPlugin.getActiveContext(),
                  skills,
                ),
              store,
              {stored},
            );
          },
          onNewConversation: () => {
            if (IntellettoPlugin.session?.loading) {
              IntellettoPlugin.session.cancel();
            }
            IntellettoPlugin.session = undefined;
          },
          onRenameConversation:
            session !== undefined && isStored
              ? (name) => session.rename(name)
              : undefined,
          onDeleteConversation: (id) => {
            store.delete(id);
            // Deleting the open conversation drops back to a fresh one.
            if (IntellettoPlugin.session?.id === id) {
              if (IntellettoPlugin.session.loading) {
                IntellettoPlugin.session.cancel();
              }
              IntellettoPlugin.session = undefined;
            }
          },
          pendingMessages: session?.queuedFollowUps ?? [],
          onDeletePendingMsg: (index) => session?.removeQueuedFollowUp(index),
          onSendPendingMsgNow: (index) => {
            if (selectedModel !== undefined) {
              session?.sendQueuedFollowUpNow(index, selectedModel);
            }
          },
          contextItems: IntellettoPlugin.getActiveContext().buildContextItems(),
          excludedContextIds: session?.excludedContext ?? EMPTY_SET,
          onToggleContext: (id) => session?.toggleContext(id),
          models,
          selectedModel,
          onSelectModel: (model) => {
            if (session !== undefined) {
              session.model = model;
              session.save();
            }
            IntellettoPlugin.selectedModelSetting.set(model);
          },
          configureModelsHref: `#!/settings/${encodeURIComponent(LLM_PROVIDER_SETTING_ID)}`,
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

  // App-scoped store of past conversations, persisted to IndexedDB and
  // shared across all traces.
  private static conversationStore: ConversationStore;

  // The shared tool registry for this trace, inheriting app-scoped tools. Core
  // tools plus any contributed by other plugins land here; the chat panel
  // hands it to the agent.
  private readonly tools = new ToolRegistry(IntellettoPlugin.appTools);

  // The shared context-provider registry for this trace. Core providers (trace,
  // page, viewport, selection) plus any contributed by other plugins; the chat
  // panel samples it for the context strip and the prompt.
  private readonly context = new ContextRegistry();

  // Queries the assistant has run, so evidence chips citing them can be
  // reopened. Trace-scoped, so ids survive starting a new chat session.
  private readonly evidence = new EvidenceStore();
  constructor(private readonly trace: Trace) {
    registerCoreTools(this.tools, this.trace, this.evidence);
    registerTimelineTools(this.tools, this.trace);
    registerSkillTools(this.tools, this.trace.skills, this.trace.engine);
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

  // Show the user the evidence behind an evidence chip they clicked.
  private openEvidence(ev: Evidence): void {
    switch (ev.kind) {
      case 'query': {
        const sql = this.evidence.getQuery(ev.id);
        if (sql === undefined) return;
        this.trace.plugins
          .getPlugin(QueryPagePlugin)
          .addQueryResultsTab({query: sql, title: `Evidence ${ev.id}`});
        return;
      }
      case 'event':
        this.trace.selection.selectSqlEvent(ev.table, ev.id, {
          scrollToSelection: true,
          switchToCurrentSelectionTab: true,
        });
        return;
      case 'range':
        this.trace.timeline.panSpanIntoView(
          Time.fromRaw(ev.start),
          Time.fromRaw(ev.end),
          {align: 'zoom'},
        );
        return;
    }
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

// The assembled system prompt: the application brief, the context providers'
// payload-format descriptions (already sorted by provider id) and the skill
// index (sorted by name), so the result is byte-identical across turns and the
// cached prefix survives. Sampled once per conversation; providers and skills
// added mid-conversation take effect on the next one.
function assembleSystemPrompt(
  context: ContextRegistry,
  skills: ReadonlyArray<SkillInfo>,
): string {
  const parts = [SYSTEM_PROMPT];
  const descriptions = context.descriptions();
  if (descriptions.length > 0) {
    parts.push(
      'Context payload formats (for the context preamble on user messages):',
      ...descriptions,
    );
  }
  const skillsSection = skillsPromptSection(skills);
  if (skillsSection !== undefined) parts.push(skillsSection);
  return parts.join('\n\n');
}

// Given a currently selected model path and the list of available agentic
// models, return either the selected model if it still exists, or the first
// available model otherwise.
function getValidModel(
  model: ModelPath | undefined,
  models: readonly ModelDetails[],
): ModelPath | undefined {
  if (model !== undefined) {
    const found = models.find(
      (m) => m.model.id === model.modelId && m.provider.id === model.providerId,
    );
    if (found !== undefined) {
      return model;
    }
  }
  const first = models[0];
  if (first !== undefined) {
    return {providerId: first.provider.id, modelId: first.model.id};
  }
  return undefined;
}

// Re-export the public API types so dependents can import them from the plugin
// entry point: `import IntellettoPlugin, {ToolRegistration} from '...'`.
export type {
  ContextProviderRegistration,
  ContextSnapshot,
  IntellettoToolRegistrar,
  ToolRegistration,
} from './api';
