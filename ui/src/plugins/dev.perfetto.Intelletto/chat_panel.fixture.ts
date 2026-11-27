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

// Component harness fixtures for ChatPanel. Only bundled into the component
// harness (ui/build --enable-component-harness), never shipped.

import m from 'mithril';
import {
  type FixtureArgs,
  type FixtureModule,
  record,
} from '../../component_harness/registry';
import type {ModelDetails, ModelPath} from '../dev.perfetto.Llm/gateway';
import {
  ChatPanel,
  type ChatPanelAttrs,
  type ConversationSummary,
} from './chat_panel';
import type {ChatLine} from './chat_session';
import type {ContextItem} from './context';

const FAKE_MODELS: readonly ModelDetails[] = [
  {
    provider: {
      id: 'gemini-prod',
      label: 'Gemini API',
      protocolName: 'Gemini',
    },
    model: {
      id: 'gemini-2.5-pro',
      label: 'Gemini 2.5 Pro',
      modelName: 'gemini-2.5-pro',
      roles: ['agentic'],
    },
  },
  {
    provider: {
      id: 'gemini-prod',
      label: 'Gemini API',
      protocolName: 'Gemini',
    },
    model: {
      id: 'gemini-2.5-flash',
      label: 'Gemini 2.5 Flash',
      modelName: 'gemini-2.5-flash',
      roles: ['agentic'],
    },
  },
  {
    provider: {
      id: 'local-ollama',
      label: 'Local Ollama',
      protocolName: 'OpenAI',
    },
    model: {
      id: 'qwen-32b',
      modelName: 'qwen2.5-coder:32b',
      roles: ['agentic'],
    },
  },
];

const DEFAULT_MODEL: ModelPath = {
  providerId: 'gemini-prod',
  modelId: 'gemini-2.5-pro',
};

const FAKE_CONTEXT: readonly ContextItem[] = [
  {
    id: 'dev.perfetto.Intelletto#page',
    label: 'Page: /viewer',
    payload: 'Current page route: /viewer',
  },
  {
    id: 'dev.perfetto.Intelletto#viewport',
    label: 'Visible time range',
    payload: JSON.stringify({
      kind: 'viewport',
      start: 100_000_000,
      end: 250_000_000,
    }),
  },
  {
    id: 'dev.perfetto.Intelletto#selection',
    label: 'Selected event',
    payload: JSON.stringify({
      kind: 'track_event',
      trackUri: '/process_1234/thread_5678',
      eventId: 42,
      ts: 142_500_000,
      dur: 18_200_000,
    }),
  },
];

const FAKE_CONVERSATIONS: readonly ConversationSummary[] = [
  {id: 'conv-1', title: 'Investigate main thread jank'},
  {id: 'conv-2', title: 'Binder latency spikes'},
  {id: 'conv-3', title: 'Startup SQL breakdown'},
];

const SAMPLE_CONVERSATION: readonly ChatLine[] = [
  {
    role: 'user',
    text: 'Why did the UI thread stall around 142ms?',
  },
  {
    role: 'thought',
    text: 'Let me inspect the slowest slices on the main thread during the visible viewport.',
  },
  {
    role: 'toolcall',
    text: 'run_sql_query',
    toolArgs: {
      sql: 'SELECT name, dur / 1e6 AS dur_ms FROM slice ORDER BY dur DESC LIMIT 3',
    },
    toolResult: JSON.stringify(
      [
        {name: 'Choreographer#doFrame', dur_ms: 18.2},
        {name: 'inflate', dur_ms: 11.4},
        {name: 'measure', dur_ms: 4.1},
      ],
      null,
      2,
    ),
  },
  {role: 'spacer', text: ''},
  {
    role: 'toolcall',
    text: 'get_track_summary',
    toolArgs: {trackUri: '/missing/track'},
    toolResult: 'Error: track "/missing/track" not found',
    toolError: true,
  },
  {role: 'spacer', text: ''},
  {
    role: 'ai',
    text: [
      'The stall is caused by a **18.2 ms** `Choreographer#doFrame` slice, mostly spent in layout inflation:',
      '',
      '| Metric | Value | Notes |',
      '| --- | --- | --- |',
      '| Process | `com.example.app` | PID `1234` |',
      '| Thread | `main` | UI thread |',
      '| Frame budget | `16.6 ms` | Exceeded by **1.6 ms** |',
      '',
      'Top slices in the stalled frame:',
      '',
      '```datagrid',
      '| Slice | Duration (ms) |',
      '| --- | --- |',
      '| Choreographer#doFrame | 18.2 |',
      '| inflate | 11.4 |',
      '| measure | 4.1 |',
      '```',
      '',
      'You can reproduce this breakdown with:',
      '',
      '```sql',
      'SELECT name, dur / 1e6 AS dur_ms',
      'FROM slice',
      'ORDER BY dur DESC',
      'LIMIT 3;',
      '```',
    ].join('\n'),
  },
  {role: 'spacer', text: ''},
];

export default {
  // Fresh conversation with no messages yet.
  empty: (args) => chatPanel(args, {}),

  // Multi-turn transcript with thoughts, tool calls, normal markdown table,
  // datagrid, code snippet, token count, excluded context chip, and
  // conversation switcher.
  conversation: (args) =>
    chatPanel(args, {
      conversation: SAMPLE_CONVERSATION,
      conversations: FAKE_CONVERSATIONS,
      activeConversationId: 'conv-1',
      totalTokens: 14_320,
      excludedContextIds: new Set(['dev.perfetto.Intelletto#viewport']),
    }),

  // Agent mid-turn with a pending tool call and cancel button.
  running: (args) =>
    chatPanel(args, {
      isAgentRunning: true,
      totalTokens: 6_400,
      conversation: [
        {role: 'user', text: 'Find all binder transactions slower than 5ms.'},
        {
          role: 'toolcall',
          text: 'run_sql_query',
          toolArgs: {
            sql: 'SELECT * FROM android_binder_txns WHERE client_dur > 5e6',
          },
        },
        {role: 'spacer', text: ''},
      ],
    }),

  // Agent running with follow-up messages queued above the prompt box.
  queued_followups: (args) =>
    chatPanel(args, {
      isAgentRunning: true,
      totalTokens: 9_850,
      conversation: [
        {role: 'user', text: 'Summarise CPU usage by process.'},
        {
          role: 'ai',
          text: 'Checking top CPU consumers across the trace...',
        },
      ],
      pendingMessages: [
        'Group the results by thread state as well',
        'Focus only on com.example.app',
      ],
    }),

  // Skills are still being indexed before the first turn.
  loading_skills: (args) =>
    chatPanel(args, {
      isLoadingSkills: true,
    }),

  // No agentic models configured in the LLM gateway.
  no_models: (args) =>
    chatPanel(args, {
      conversations: FAKE_CONVERSATIONS,
      activeConversationId: 'conv-1',
      models: [],
      selectedModel: undefined,
    }),

  // Transcript ending in a retryable error message.
  error: (args) =>
    chatPanel(args, {
      conversation: [
        {role: 'user', text: 'Explain the GPU stall at 3.2s.'},
        {
          role: 'error',
          text: 'Something went wrong: Error: HTTP 429 Too Many Requests',
        },
      ],
      onRetry: () => record('retry'),
    }),
} satisfies FixtureModule;

// Renders ChatPanel statically inside a sidebar-sized box. All callbacks only
// record events for tests; no fixture state is mutated.
//
// Args:
//   width=<css>   container width (default '420px')
//   height=<css>  container height (default '640px')
function chatPanel(
  args: FixtureArgs,
  overrides: Partial<ChatPanelAttrs>,
): m.Component {
  const attrs: ChatPanelAttrs = {
    isAgentRunning: false,
    isLoadingSkills: false,
    totalTokens: undefined,
    onCancel: () => record('cancel'),
    conversation: [],
    onSendMessage: (msg) => record({sendMessage: msg}),
    conversations: [],
    activeConversationId: undefined,
    onOpenConversation: (id) => record({openConversation: id}),
    onNewConversation: () => record('newConversation'),
    pendingMessages: [],
    onDeletePendingMsg: (index) => record({deletePendingMsg: index}),
    onSendPendingMsgNow: (index) => record({sendPendingMsgNow: index}),
    contextItems: FAKE_CONTEXT,
    excludedContextIds: new Set(),
    onToggleContext: (id) => record({toggleContext: id}),
    models: FAKE_MODELS,
    selectedModel: DEFAULT_MODEL,
    onSelectModel: (model) => record({selectModel: model}),
    ...overrides,
  };

  return {
    view: () =>
      m(
        'div',
        {
          style: {
            width: args.width ?? '420px',
            height: args.height ?? '640px',
          },
          onclick: (e: MouseEvent) => {
            const target = e.target as Element | null;
            const link = target?.closest('a[href]');
            if (link === null || link === undefined) return;
            e.preventDefault();
            record({openUrl: link.getAttribute('href')});
          },
        },
        m(ChatPanel, attrs),
      ),
  };
}
