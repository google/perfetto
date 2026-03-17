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
import {z} from 'zod';
import {App} from '../../public/app';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {Setting} from '../../public/settings';
import {LlmProvider} from './provider';
import {AnthropicProvider, listAnthropicModels} from './anthropic';
import {GeminiProvider, listGeminiModels} from './gemini';
import {createTools, registerWebMcpTools} from './tools';
import {Selection} from '../../public/selection';
import {Raf} from '../../public/raf';
import {Button} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';
import {Icon} from '../../widgets/icon';
import DataExplorerPlugin from '../dev.perfetto.DataExplorer';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {maybeUndefined} from '../../base/utils';

type ProviderType = 'anthropic' | 'gemini';

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

The trace processor uses SQLite with perfetto extensions. Key tables:
- slice: function calls/events (ts, dur, name, track_id, category, depth)
- thread/process: thread and process metadata
- sched_slice: CPU scheduling events
- counter: counter track values
- android_logs: logcat messages (ts, prio, tag, msg)

Use 'INCLUDE PERFETTO MODULE <name>' (in a separate query) to load stdlib modules.

You can also interact with the Node Query Builder (Data Explorer), a visual
query builder:
- Use get_query_builder_graph to see the user's current analysis pipeline
- Use set_query_builder_graph to create or modify analysis pipelines
  (the tool description contains the full graph JSON format reference)
- Use select_query_builder_node to select a node and show its results
- The Data Explorer page must be open (#!/explore) for graph tools to work
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

export default class AiAssistantPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Intelletto';
  static readonly dependencies = [DataExplorerPlugin, SqlModulesPlugin];
  static readonly description =
    'AI assistant that can query traces and interact with the UI via the omnibox.';

  static providerSetting: Setting<ProviderType>;
  static apiKeySetting: Setting<string>;
  static modelSetting: Setting<string>;

  static onActivate(app: App): void {
    AiAssistantPlugin.providerSetting = app.settings.register({
      id: `${AiAssistantPlugin.id}#Provider`,
      name: 'AI Provider',
      description: 'Which LLM provider to use.',
      schema: z.enum(['gemini', 'anthropic']),
      defaultValue: 'gemini' as ProviderType,
    });

    AiAssistantPlugin.apiKeySetting = app.settings.register({
      id: `${AiAssistantPlugin.id}#ApiKey`,
      name: 'AI API Key',
      description: 'API key for the selected provider (Anthropic or Gemini).',
      schema: z.string().meta({secret: true}),
      defaultValue: '',
    });

    AiAssistantPlugin.modelSetting = app.settings.register({
      id: `${AiAssistantPlugin.id}#Model`,
      name: 'AI Model',
      description:
        'Model name (E.g. gemini-2.5-pro for Gemini, claude-sonnet-4-20250514 for Anthropic).',
      schema: z.string(),
      defaultValue: 'gemini-2.5-flash',
    });
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

    function createProvider(): LlmProvider {
      const apiKey = AiAssistantPlugin.apiKeySetting.get();
      const model = AiAssistantPlugin.modelSetting.get();
      const providerType = AiAssistantPlugin.providerSetting.get();
      switch (providerType) {
        case 'gemini':
          return new GeminiProvider(apiKey, model, SYSTEM_PROMPT);
        case 'anthropic':
        default:
          return new AnthropicProvider(apiKey, model, SYSTEM_PROMPT);
      }
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
      const apiKey = AiAssistantPlugin.apiKeySetting.get();
      if (!apiKey) {
        turns.push({
          userPrompt: prompt,
          response:
            'No API key configured. Go to Settings and set your AI API Key.',
          toolCalls: [],
        });
        trace.sidePanel.showTab(SIDE_PANEL_ID);
        raf.scheduleFullRedraw();
        return;
      }

      if (isRunning) return;

      // Create provider lazily (or reuse for follow-ups)
      if (!provider) {
        provider = createProvider();
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
          m(
            '.pf-ai-assistant__user',
            m('span.pf-ai-assistant__label', 'You'),
            turn.userPrompt,
          ),
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
      );
    }

    // Register the side panel tab
    trace.sidePanel.registerTab({
      uri: SIDE_PANEL_ID,
      title: 'Intelletto',
      icon: 'app_spark',
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
                    icon: 'app_spark',
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
      id: `${AiAssistantPlugin.id}#Activate`,
      name: 'Ask AI about this trace',
      callback: () => {
        trace.sidePanel.showTab(SIDE_PANEL_ID);
      },
      defaultHotkey: '!Mod+Shift+A',
    });

    trace.commands.registerCommand({
      id: `${AiAssistantPlugin.id}#Reset`,
      name: 'AI: New conversation',
      callback: resetConversation,
    });

    trace.commands.registerCommand({
      id: `${AiAssistantPlugin.id}#SelectModel`,
      name: 'AI: Select model',
      callback: async () => {
        const apiKey = AiAssistantPlugin.apiKeySetting.get();
        if (!apiKey) {
          await trace.omnibox.prompt(
            'No API key configured. Set it in Settings first.',
          );
          return;
        }

        const providerType = AiAssistantPlugin.providerSetting.get();
        try {
          let models: {id: string; name: string}[];
          if (providerType === 'gemini') {
            const raw = await listGeminiModels(apiKey);
            models = raw.map((m) => ({
              id: m.name.replace('models/', ''),
              name: m.displayName,
            }));
          } else {
            const raw = await listAnthropicModels(apiKey);
            models = raw.map((m) => ({
              id: m.id,
              name: m.display_name,
            }));
          }

          const selected = await trace.omnibox.prompt('Select a model', {
            values: models,
            getName: (m) => `${m.name} (${m.id})`,
          });
          if (selected) {
            AiAssistantPlugin.modelSetting.set(selected.id);
            resetConversation();
          }
        } catch (e) {
          await trace.omnibox.prompt(`Failed to list models: ${e}`);
        }
      },
    });

    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Intelletto',
      icon: 'app_spark',
      sortOrder: 9,
      action: () => {
        trace.sidePanel.showTab(SIDE_PANEL_ID);
      },
    });
  }
}
