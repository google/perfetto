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

import './styles.scss';
import m from 'mithril';
import {z} from 'zod';
import type {App} from '../../public/app';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Setting} from '../../public/settings';
import type {LlmProvider, Protocol, ProviderConfig} from './provider';
import {anthropicProtocol} from './anthropic';
import {createTools, registerWebMcpTools} from './tools';
import type {Selection} from '../../public/selection';
import type {Raf} from '../../public/raf';
import {Button} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';
import {Icon} from '../../widgets/icon';
import {TextInput} from '../../widgets/text_input';
import {Select} from '../../widgets/select';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import SpaghettiPlugin from '../dev.perfetto.Spaghetti';
import {maybeUndefined} from '../../base/utils';

// Zod schema for the user-configured providers map.
const providerConfigSchema = z.object({
  api: z.string(),
  apiKey: z.string().meta({secret: true}),
  baseUrl: z.string().optional(),
  models: z
    .array(
      z.object({
        id: z.string(),
        input: z.array(z.string()).optional(),
      }),
    )
    .optional(),
});
const providersSchema = z.record(z.string(), providerConfigSchema);
type ProvidersConfig = z.infer<typeof providersSchema>;

const selectedModelSchema = z.object({
  providerId: z.string(),
  modelId: z.string(),
});
type SelectedModel = z.infer<typeof selectedModelSchema>;

const SYSTEM_PROMPT = `You are an expert performance analyst embedded in the Perfetto trace viewer UI.
You help users understand their trace data by querying the trace processor and
highlighting relevant events in the timeline.

When answering questions:
- Use the execute_query tool to look up data from the trace
- Use get_selection to understand what the user is currently looking at
- Prefer making selections (select_event, select_track) over just scrolling - selections highlight the item and show details, making it easier for the user to see what you found
- Use list_tracks to discover track URIs and select_track to highlight tracks
- Be concise - your response appears in a small panel
- Format key findings as short bullet points
- Include specific numbers (timestamps, durations, counts) when relevant

IMPORTANT — always use tools, never training knowledge, for Perfetto internals:
- Use list_sql_tables to discover available tables and views — do NOT assume
  table names, column names, or module paths from your training data
- Use get_table_schema before querying any table you haven't already looked up
  in this session
- Use 'INCLUDE PERFETTO MODULE <name>' (in a separate prior query) to load
  stdlib modules — get the correct include key from get_table_schema, not from
  memory
- Perfetto's stdlib evolves; your training data may be stale or wrong

You can interact with two visual query builders:

**Spaghetti** (preferred for new analysis pipelines):
- Use get_spaghetti_graph to see the user's current graph
- Use validate_spaghetti_graph to check a graph before applying it
- Use set_spaghetti_graph to create or modify analysis pipelines
  (the tool description contains the full graph JSON format reference and
  validates automatically before applying — errors are returned without
  changing anything)
- Use select_spaghetti_node to select a node and show its results
- Navigate to #!/spaghetti first; the page must be open for graph tools to work
- Supports: from, time_range, select, filter, sort, limit, groupby, join,
  union, interval_intersect, extract_arg, chart nodes
- The "chart" node type shows bar charts in the details panel instead of a table

`;

const SIDE_PANEL_ID = 'dev.perfetto.IntellettoChat';

interface ToolCall {
  readonly name: string;
  readonly input: string;
  error?: string; // Set when the tool call throws or returns an error
}

// A visible conversation turn (user question + assistant answer).
interface ConversationTurn {
  readonly userPrompt: string;
  response: string;
  readonly toolCalls: ToolCall[];
}

export default class IntellettoPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Intelletto';
  static readonly dependencies = [SqlModulesPlugin, SpaghettiPlugin];
  static readonly description =
    'AI assistant that can query traces and interact with the UI via the omnibox.';

  static providersSetting: Setting<ProvidersConfig>;
  static selectedSetting: Setting<SelectedModel>;

  // Registry of protocols (e.g. anthropic, gemini, openai-compatible).
  // Other plugins can extend this via registerProtocol().
  private static readonly protocols = new Map<string, Protocol>();

  // Register a new model-API protocol. Safe to call from any plugin's
  // onActivate or onTraceLoad. Overwrites any existing protocol with the
  // same name.
  static registerProtocol(protocol: Protocol): void {
    IntellettoPlugin.protocols.set(protocol.name, protocol);
  }

  static getProtocol(name: string): Protocol | undefined {
    return IntellettoPlugin.protocols.get(name);
  }

  static listProtocols(): ReadonlyArray<Protocol> {
    return Array.from(IntellettoPlugin.protocols.values());
  }

  static onActivate(app: App): void {
    // Built-in protocols.
    IntellettoPlugin.registerProtocol(anthropicProtocol);

    IntellettoPlugin.providersSetting = app.settings.register({
      id: `${IntellettoPlugin.id}#Providers`,
      name: 'AI Providers',
      description:
        'Map of provider id -> {api, apiKey, baseUrl?, models?[]}. ' +
        '"api" must match a registered protocol (e.g. anthropic, gemini, ' +
        'openai-compatible).',
      schema: providersSchema,
      defaultValue: {
        gemini: {
          api: 'gemini',
          apiKey: '',
        },
      } as ProvidersConfig,
      render: renderProvidersSetting,
    });

    IntellettoPlugin.selectedSetting = app.settings.register({
      id: `${IntellettoPlugin.id}#Selected`,
      name: 'AI Model',
      description:
        'Selected provider and model. ' +
        'Use the "AI: Select model" command to pick interactively.',
      schema: selectedModelSchema,
      defaultValue: {providerId: '', modelId: ''} as SelectedModel,
    });

    app.commands.registerCommand({
      id: `${IntellettoPlugin.id}#SelectModel`,
      name: 'AI: Select model',
      callback: async () => {
        const providers = IntellettoPlugin.providersSetting.get();
        const providerIds = Object.keys(providers);
        if (providerIds.length === 0) {
          await app.omnibox.prompt(
            'No providers configured. Add one in Settings (AI Providers).',
          );
          return;
        }

        type Entry = {
          providerId: string;
          modelId: string;
          displayName?: string;
        };
        const entries: Entry[] = [];
        const errors: string[] = [];

        await Promise.all(
          providerIds.map(async (providerId) => {
            const cfg = maybeUndefined(providers[providerId]);
            if (!cfg) return;
            // Seed with any user-declared models so they show up even if
            // the remote endpoint is unreachable.
            for (const m of cfg.models ?? []) {
              entries.push({providerId, modelId: m.id});
            }
            const protocol = IntellettoPlugin.getProtocol(cfg.api);
            if (!protocol) {
              errors.push(`${providerId}: unknown protocol "${cfg.api}"`);
              return;
            }
            try {
              const models = await protocol.listModels(cfg);
              for (const m of models) {
                if (
                  entries.some(
                    (e) => e.providerId === providerId && e.modelId === m.id,
                  )
                ) {
                  continue;
                }
                entries.push({
                  providerId,
                  modelId: m.id,
                  displayName: m.displayName,
                });
              }
            } catch (e) {
              errors.push(`${providerId}: ${e}`);
            }
          }),
        );

        if (entries.length === 0) {
          await app.omnibox.prompt(
            `No models found. ${errors.join('; ') || ''}`,
          );
          return;
        }

        const selected = await app.omnibox.prompt('Select a model', {
          values: entries,
          getName: (e) =>
            e.displayName
              ? `${e.providerId}:${e.modelId} (${e.displayName})`
              : `${e.providerId}:${e.modelId}`,
        });
        if (selected) {
          IntellettoPlugin.selectedSetting.set({
            providerId: selected.providerId,
            modelId: selected.modelId,
          });
          // Best-effort reset of the live per-trace conversation.
          try {
            app.commands.runCommand(`${IntellettoPlugin.id}#Reset`);
          } catch {
            // No trace loaded — nothing to reset.
          }
        }
      },
    });
  }

  // Instance API: convenience wrapper so plugins that depend on Intelletto
  // can register protocols via trace.plugins.getPlugin(...).registerProtocol().
  registerProtocol(protocol: Protocol): void {
    IntellettoPlugin.registerProtocol(protocol);
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    const tools = createTools(trace);
    registerWebMcpTools(tools);
    let abortController: AbortController | undefined;
    let isRunning = false;
    let toolStatus = '';
    let hitToolLimit = false;
    let provider: LlmProvider | undefined;
    let inputText = '';
    const raf: Raf = trace.raf;

    const turns: ConversationTurn[] = [];
    const expandedUserPrompts = new Set<number>();

    function resolveSelected(): {
      providerId: string;
      modelId: string;
      cfg: ProviderConfig;
      protocol: Protocol;
    } {
      const {providerId, modelId} = IntellettoPlugin.selectedSetting.get();
      if (!providerId || !modelId) {
        throw new Error(
          'No model selected. Run "AI: Select model" to pick one.',
        );
      }
      const providers = IntellettoPlugin.providersSetting.get();
      const cfg = maybeUndefined(providers[providerId]);
      if (!cfg) {
        throw new Error(`Provider "${providerId}" is not configured.`);
      }
      const protocol = IntellettoPlugin.getProtocol(cfg.api);
      if (!protocol) {
        throw new Error(`Unknown protocol "${cfg.api}".`);
      }
      return {providerId, modelId, cfg, protocol};
    }

    function createProvider(): LlmProvider {
      const {modelId, cfg, protocol} = resolveSelected();
      return protocol.createProvider(cfg, modelId, SYSTEM_PROMPT);
    }

    let conversationTitle = '';

    function resetConversation() {
      provider = undefined;
      turns.length = 0;
      toolStatus = '';
      hitToolLimit = false;
      conversationTitle = '';
      raf.scheduleFullRedraw();
    }

    function describeSelection(sel: Selection): string | undefined {
      switch (sel.kind) {
        case 'track_event':
          return `track event (trackUri=${sel.trackUri}, eventId=${sel.eventId})`;
        case 'area':
          return `time range ${Number(sel.start)}ns - ${Number(sel.end)}ns across ${sel.trackUris.length} track(s)`;
        case 'track':
          return `track (uri=${sel.trackUri})`;
        default:
          return undefined;
      }
    }

    async function submitPrompt(prompt: string) {
      if (isRunning) return;

      // Create provider lazily (or reuse for follow-ups)
      if (!provider) {
        try {
          provider = createProvider();
        } catch (e) {
          turns.push({
            userPrompt: prompt,
            response: `${e}`,
            toolCalls: [],
          });
          trace.sidePanel.showTab(SIDE_PANEL_ID);
          raf.scheduleFullRedraw();
          return;
        }
      }

      // Augment the prompt with the current selection context.
      const selDesc = describeSelection(trace.selection.selection);
      const augmentedPrompt = selDesc
        ? `${prompt}\n\n[Current selection: ${selDesc}]`
        : prompt;

      isRunning = true;
      toolStatus = '';
      abortController = new AbortController();

      // Set conversation title from the first prompt.
      if (turns.length === 0) {
        const maxLen = 40;
        conversationTitle =
          prompt.length > maxLen ? prompt.slice(0, maxLen) + '...' : prompt;
      }

      const turn: ConversationTurn = {
        userPrompt: prompt,
        response: '',
        toolCalls: [],
      };
      turns.push(turn);

      trace.sidePanel.showTab(SIDE_PANEL_ID);
      raf.scheduleFullRedraw();

      try {
        const result = await provider.sendMessage({
          userPrompt: augmentedPrompt,
          tools,
          signal: abortController.signal,
          onText: (text) => {
            turn.response = text;
            raf.scheduleFullRedraw();
          },
          onToolUse: (name, input) => {
            toolStatus = `Using ${name}...`;
            turn.toolCalls.push({name, input});
            raf.scheduleFullRedraw();
          },
          onToolResult: (_name, error) => {
            if (error) {
              const lastTc = maybeUndefined(
                turn.toolCalls[turn.toolCalls.length - 1],
              );
              if (lastTc) lastTc.error = error;
              raf.scheduleFullRedraw();
            }
          },
        });
        hitToolLimit = result.hitToolLimit;
      } catch (e) {
        if ((e as Error).name !== 'AbortError') {
          turn.response = `Error: ${e}`;
        }
      } finally {
        isRunning = false;
        toolStatus = '';
        raf.scheduleFullRedraw();
      }
    }

    async function continueAfterLimit() {
      if (!provider || isRunning) return;

      isRunning = true;
      hitToolLimit = false;
      toolStatus = '';
      abortController = new AbortController();

      const turn = turns[turns.length - 1];
      raf.scheduleFullRedraw();

      try {
        const result = await provider.continueToolUse({
          tools,
          signal: abortController.signal,
          onText: (text) => {
            turn.response = text;
            raf.scheduleFullRedraw();
          },
          onToolUse: (name, input) => {
            toolStatus = `Using ${name}...`;
            turn.toolCalls.push({name, input});
            raf.scheduleFullRedraw();
          },
          onToolResult: (_name, error) => {
            if (error) {
              const lastTc = maybeUndefined(
                turn.toolCalls[turn.toolCalls.length - 1],
              );
              if (lastTc) lastTc.error = error;
              raf.scheduleFullRedraw();
            }
          },
        });
        hitToolLimit = result.hitToolLimit;
      } catch (e) {
        if ((e as Error).name !== 'AbortError') {
          turn.response += `\nError: ${e}`;
        }
      } finally {
        isRunning = false;
        toolStatus = '';
        raf.scheduleFullRedraw();
      }
    }

    function renderConversation(): m.Children {
      return turns.map((turn, i) =>
        m(
          '.pf-ai-assistant__turn',
          {key: i},
          (() => {
            const MAX_LINES = 5;
            const lines = turn.userPrompt.split('\n');
            const isLong = lines.length > MAX_LINES;
            const isExpanded = expandedUserPrompts.has(i);
            const text =
              isLong && !isExpanded
                ? lines.slice(0, MAX_LINES).join('\n') + '\u2026'
                : turn.userPrompt;
            return m('.pf-ai-assistant__user', [
              m('span.pf-ai-assistant__label', 'You'),
              text,
              isLong &&
                m(Button, {
                  label: isExpanded ? 'Show less' : 'Show more',
                  compact: true,
                  className: 'pf-ai-assistant__expand-btn',
                  onclick: () => {
                    isExpanded
                      ? expandedUserPrompts.delete(i)
                      : expandedUserPrompts.add(i);
                    raf.scheduleFullRedraw();
                  },
                }),
            ]);
          })(),
          turn.toolCalls.length > 0 &&
            m(
              '.pf-ai-assistant__tool-calls',
              turn.toolCalls.map((tc) =>
                m(
                  '.pf-ai-assistant__tool-call',
                  m('span.pf-ai-assistant__tool-name', tc.name),
                  m('pre.pf-ai-assistant__tool-args', tc.input),
                  tc.error &&
                    m(
                      '.pf-ai-assistant__tool-error',
                      {style: {color: 'var(--pf-color-danger)'}},
                      tc.error,
                    ),
                ),
              ),
            ),
          m(
            '.pf-ai-assistant__assistant',
            m('span.pf-ai-assistant__label', 'AI'),
            turn.response ||
              (isRunning && i === turns.length - 1 ? 'Thinking...' : ''),
          ),
        ),
      );
    }

    function renderInput(): m.Children {
      const selDesc = describeSelection(trace.selection.selection);
      return m(
        '.pf-ai-assistant__input-area',
        m(
          '.pf-ai-assistant__input-bar',
          m('textarea.pf-ai-assistant__input', {
            placeholder:
              turns.length > 0
                ? 'Ask a follow-up...'
                : 'Ask about this trace...',
            value: inputText,
            disabled: isRunning,
            rows: 1,
            oninput: (e: InputEvent) => {
              const el = e.target as HTMLTextAreaElement;
              inputText = el.value;
              // Auto-resize
              el.style.height = 'auto';
              el.style.height = Math.min(el.scrollHeight, 120) + 'px';
            },
            onkeydown: (e: KeyboardEvent) => {
              if (e.key === 'Enter' && !e.shiftKey) {
                e.preventDefault();
                const text = inputText.trim();
                if (text && !isRunning) {
                  inputText = '';
                  const el = e.target as HTMLTextAreaElement;
                  el.style.height = 'auto';
                  submitPrompt(text);
                }
              }
            },
          }),
          m(Button, {
            icon: isRunning ? 'stop' : 'send',
            compact: true,
            className: 'pf-ai-assistant__send-btn',
            disabled: isRunning ? false : inputText.trim().length === 0,
            onclick: () => {
              if (isRunning) {
                abortController?.abort();
                return;
              }
              const text = inputText.trim();
              if (text) {
                inputText = '';
                submitPrompt(text);
              }
            },
          }),
        ),
        selDesc
          ? m(
              '.pf-ai-assistant__selection-context',
              m(Icon, {
                icon: 'my_location',
                className: 'pf-ai-assistant__selection-icon',
              }),
              m('span', selDesc),
            )
          : undefined,
        renderModelBadge(),
      );
    }

    function renderModelBadge(): m.Children {
      const {providerId, modelId} = IntellettoPlugin.selectedSetting.get();
      const label =
        providerId && modelId
          ? `${providerId}:${modelId}`
          : 'no model selected';
      return m(
        '.pf-ai-assistant__model-badge',
        {
          style: {
            cursor: 'pointer',
            opacity: '0.7',
            fontSize: '11px',
            display: 'flex',
            alignItems: 'center',
            gap: '4px',
            marginTop: '2px',
          },
          title: 'Click to change model',
          onclick: () => {
            trace.commands.runCommand(`${IntellettoPlugin.id}#SelectModel`);
          },
        },
        m(Icon, {icon: 'stars_2'}),
        m('span', label),
      );
    }

    // Register the side panel tab
    trace.sidePanel.registerTab({
      uri: SIDE_PANEL_ID,
      title: 'Intelletto',
      icon: 'stars_2',
      render: () => {
        return m(
          '.pf-ai-assistant',
          turns.length > 0
            ? m(
                '.pf-ai-assistant__header',
                m('.pf-ai-assistant__header-title', conversationTitle),
                m(Button, {
                  icon: 'add_comment',
                  title: 'New conversation',
                  compact: true,
                  onclick: resetConversation,
                }),
              )
            : undefined,
          m(
            '.pf-ai-assistant__messages',
            turns.length === 0 && !isRunning
              ? m(
                  EmptyState,
                  {
                    icon: 'stars_2',
                    title: 'Intelletto',
                    fillHeight: true,
                  },
                  'Ask a question about this trace.',
                )
              : [
                  renderConversation(),
                  isRunning && toolStatus
                    ? m('.pf-ai-assistant__status', toolStatus)
                    : undefined,
                  !isRunning && hitToolLimit
                    ? m(
                        '.pf-ai-assistant__tool-limit',
                        m('span', 'Paused — reached tool call limit.'),
                        m(Button, {
                          label: 'Continue',
                          icon: 'play_arrow',
                          compact: true,
                          onclick: continueAfterLimit,
                        }),
                      )
                    : undefined,
                ],
          ),
          renderInput(),
        );
      },
    });

    trace.omnibox.registerMode({
      trigger: '@',
      hint: "'@' for AI assistant",
      placeholder:
        turns.length > 0
          ? 'Ask a follow-up question...'
          : 'Ask the AI about this trace...',
      className: 'pf-omnibox--ai-mode',
      closeOnSubmit: true,
      onSubmit: (prompt) => submitPrompt(prompt),
      onClose: () => {
        abortController?.abort();
      },
    });

    trace.commands.registerCommand({
      id: `${IntellettoPlugin.id}#Activate`,
      name: 'Ask AI about this trace',
      callback: () => {
        trace.sidePanel.showTab(SIDE_PANEL_ID);
      },
      defaultHotkey: '!Mod+Shift+A',
    });

    trace.commands.registerCommand({
      id: `${IntellettoPlugin.id}#Reset`,
      name: 'AI: New conversation',
      callback: resetConversation,
    });

    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Intelletto',
      icon: 'stars_2',
      sortOrder: 9,
      action: () => {
        trace.sidePanel.showTab(SIDE_PANEL_ID);
      },
    });
  }
}

// Structured form editor for the providers map. Implemented as a closure
// component so the in-flight draft and per-provider expansion state survive
// redraws.
interface ProvidersEditorAttrs {
  setting: Setting<ProvidersConfig>;
}

// Local draft shape: arrays instead of records so the user can type a new
// provider id without it disappearing as soon as it collides with the
// existing key (and so we can preserve insertion order).
interface DraftModel {
  id: string;
  input: string; // comma-separated list, e.g. "text,image"
}
interface DraftProvider {
  providerId: string;
  api: string;
  apiKey: string;
  baseUrl: string;
  models: DraftModel[];
  expanded: boolean;
}

function toDraft(cfg: ProvidersConfig): DraftProvider[] {
  return Object.entries(cfg).map(([providerId, p]) => ({
    providerId,
    api: p.api,
    apiKey: p.apiKey,
    baseUrl: p.baseUrl ?? '',
    models: (p.models ?? []).map((m) => ({
      id: m.id,
      input: (m.input ?? []).join(','),
    })),
    expanded: false,
  }));
}

function fromDraft(draft: DraftProvider[]): {
  cfg: ProvidersConfig;
  error?: string;
} {
  const out: ProvidersConfig = {};
  const seen = new Set<string>();
  for (const p of draft) {
    const id = p.providerId.trim();
    if (!id) return {cfg: out, error: 'Provider id cannot be empty.'};
    if (seen.has(id)) return {cfg: out, error: `Duplicate provider id "${id}".`};
    seen.add(id);
    if (!p.api.trim()) {
      return {cfg: out, error: `Provider "${id}": protocol is required.`};
    }
    const models = p.models
      .map((m) => ({
        id: m.id.trim(),
        input: m.input
          .split(',')
          .map((s) => s.trim())
          .filter((s) => s.length > 0),
      }))
      .filter((m) => m.id.length > 0)
      .map((m) => (m.input.length > 0 ? m : {id: m.id}));
    out[id] = {
      api: p.api.trim(),
      apiKey: p.apiKey,
      ...(p.baseUrl.trim() ? {baseUrl: p.baseUrl.trim()} : {}),
      ...(models.length > 0 ? {models} : {}),
    };
  }
  return {cfg: out};
}

const ProvidersEditor: m.ClosureComponent<ProvidersEditorAttrs> = ({attrs}) => {
  let draft: DraftProvider[] = toDraft(attrs.setting.get());
  let status: {kind: 'ok' | 'err'; msg: string} | undefined;
  let revealKey: Record<number, boolean> = {};

  function markDirty() {
    status = undefined;
  }

  function save(setting: Setting<ProvidersConfig>) {
    const {cfg, error} = fromDraft(draft);
    if (error) {
      status = {kind: 'err', msg: error};
      return;
    }
    try {
      const parsed = providersSchema.parse(cfg);
      setting.set(parsed);
      status = {kind: 'ok', msg: 'Saved.'};
    } catch (e) {
      status = {kind: 'err', msg: `${e}`};
    }
  }

  function revert(setting: Setting<ProvidersConfig>) {
    draft = toDraft(setting.get());
    revealKey = {};
    status = undefined;
  }

  function renderProvider(p: DraftProvider, idx: number): m.Children {
    const protocols = IntellettoPlugin.listProtocols();
    const knownApi = protocols.some((proto) => proto.name === p.api);
    return m(
      '.pf-intelletto-provider',
      {
        key: idx,
        style: {
          border: '1px solid var(--pf-color-border)',
          borderRadius: '6px',
          padding: '8px',
          marginBottom: '8px',
          background: 'var(--pf-color-surface)',
        },
      },
      m(
        '.pf-intelletto-provider__header',
        {
          style: {
            display: 'flex',
            alignItems: 'center',
            gap: '6px',
            marginBottom: p.expanded ? '8px' : '0',
          },
        },
        m(Button, {
          icon: p.expanded ? 'expand_more' : 'chevron_right',
          compact: true,
          onclick: () => {
            p.expanded = !p.expanded;
          },
        }),
        m(TextInput, {
          placeholder: 'provider id (e.g. anthropic-prod)',
          value: p.providerId,
          onInput: (v: string) => {
            p.providerId = v;
            markDirty();
          },
          style: {flex: '1'},
        }),
        m('span', {style: {fontSize: '11px', opacity: 0.7}}, p.api || '—'),
        m(Button, {
          icon: 'delete',
          compact: true,
          title: 'Remove provider',
          onclick: () => {
            draft.splice(idx, 1);
            markDirty();
          },
        }),
      ),
      p.expanded &&
        m(
          '.pf-intelletto-provider__body',
          {style: {display: 'flex', flexDirection: 'column', gap: '6px'}},
          m(
            'label',
            {style: {display: 'flex', flexDirection: 'column', gap: '2px'}},
            m('span', {style: {fontSize: '11px', opacity: 0.7}}, 'Protocol'),
            m(
              Select,
              {
                value: p.api,
                onchange: (e: Event) => {
                  p.api = (e.target as HTMLSelectElement).value;
                  markDirty();
                },
              },
              [
                !knownApi && p.api
                  ? m('option', {value: p.api}, `${p.api} (unregistered)`)
                  : undefined,
                p.api ? undefined : m('option', {value: ''}, '— select —'),
                ...protocols.map((proto) =>
                  m('option', {value: proto.name}, proto.name),
                ),
              ],
            ),
          ),
          m(
            'label',
            {style: {display: 'flex', flexDirection: 'column', gap: '2px'}},
            m('span', {style: {fontSize: '11px', opacity: 0.7}}, 'API key'),
            m(
              '.pf-intelletto-apikey-row',
              {style: {display: 'flex', gap: '4px'}},
              m(TextInput, {
                type: revealKey[idx] ? 'text' : 'password',
                value: p.apiKey,
                placeholder: 'sk-...',
                onInput: (v: string) => {
                  p.apiKey = v;
                  markDirty();
                },
                style: {flex: '1'},
              }),
              m(Button, {
                icon: revealKey[idx] ? 'visibility_off' : 'visibility',
                compact: true,
                title: revealKey[idx] ? 'Hide' : 'Show',
                onclick: () => {
                  revealKey[idx] = !revealKey[idx];
                },
              }),
            ),
          ),
          m(
            'label',
            {style: {display: 'flex', flexDirection: 'column', gap: '2px'}},
            m(
              'span',
              {style: {fontSize: '11px', opacity: 0.7}},
              'Base URL (optional)',
            ),
            m(TextInput, {
              value: p.baseUrl,
              placeholder: 'https://api.example.com',
              onInput: (v: string) => {
                p.baseUrl = v;
                markDirty();
              },
            }),
          ),
          m(
            '.pf-intelletto-models',
            m(
              'span',
              {style: {fontSize: '11px', opacity: 0.7}},
              'Models (optional — seed list shown in picker even when offline)',
            ),
            p.models.map((mdl, mIdx) =>
              m(
                '.pf-intelletto-model-row',
                {
                  key: mIdx,
                  style: {
                    display: 'flex',
                    gap: '4px',
                    marginTop: '4px',
                    alignItems: 'center',
                  },
                },
                m(TextInput, {
                  placeholder: 'model id',
                  value: mdl.id,
                  onInput: (v: string) => {
                    mdl.id = v;
                    markDirty();
                  },
                  style: {flex: '2'},
                }),
                m(TextInput, {
                  placeholder: 'input kinds (text,image)',
                  value: mdl.input,
                  onInput: (v: string) => {
                    mdl.input = v;
                    markDirty();
                  },
                  style: {flex: '3'},
                }),
                m(Button, {
                  icon: 'delete',
                  compact: true,
                  title: 'Remove model',
                  onclick: () => {
                    p.models.splice(mIdx, 1);
                    markDirty();
                  },
                }),
              ),
            ),
            m(Button, {
              label: 'Add model',
              icon: 'add',
              compact: true,
              className: 'pf-intelletto-add-model',
              onclick: () => {
                p.models.push({id: '', input: ''});
                markDirty();
              },
            }),
          ),
        ),
    );
  }

  return {
    view: ({attrs: a}) =>
      m(
        '.pf-intelletto-providers-editor',
        draft.map((p, i) => renderProvider(p, i)),
        m(
          '.pf-intelletto-providers-editor__actions',
          {style: {marginTop: '8px', display: 'flex', gap: '8px'}},
          m(Button, {
            label: 'Add provider',
            icon: 'add',
            compact: true,
            onclick: () => {
              const protocols = IntellettoPlugin.listProtocols();
              draft.push({
                providerId: '',
                api: protocols[0]?.name ?? '',
                apiKey: '',
                baseUrl: '',
                models: [],
                expanded: true,
              });
              markDirty();
            },
          }),
          m(Button, {
            label: 'Save',
            compact: true,
            onclick: () => save(a.setting),
          }),
          m(Button, {
            label: 'Revert',
            compact: true,
            onclick: () => revert(a.setting),
          }),
        ),
        status &&
          m(
            '.pf-intelletto-providers-editor__status',
            {
              style: {
                color: status.kind === 'err' ? 'red' : 'green',
                marginTop: '4px',
                whiteSpace: 'pre-wrap',
              },
            },
            status.msg,
          ),
      ),
  };
};

function renderProvidersSetting(setting: Setting<ProvidersConfig>): m.Children {
  return m(ProvidersEditor, {setting});
}
