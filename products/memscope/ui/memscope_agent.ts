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

import type {App} from '../../../ui/src/public/app';
import {processRouteArg} from '../../../ui/src/plugins/dev.perfetto.Memscope/process_focus';
import {createProductAgentPlugin} from '../../common/ui/agent/product_agent_plugin';
import type {
  AgentContextSnapshot,
  ProductAgentAdapter,
  SuggestedPrompt,
} from '../../common/ui/agent/types';

export const MEMSCOPE_AGENT_PLUGIN_ID = 'perfetto.product.memscope.agent';

function buildMemscopeAdapter(app: App): ProductAgentAdapter {
  return {
    productId: 'memscope',
    productName: 'Memscope',
    manifestKey: 'memscope/tools.json',

    getContext(): AgentContextSnapshot {
      const route = app.getCurrentRoute();
      const focusProcess = processRouteArg(route.args);
      const trace = app.trace;
      if (trace) {
        return {
          mode: 'trace',
          label: focusProcess
            ? `Memory Explorer · ${focusProcess}`
            : `Memory Explorer · ${trace.traceInfo.traceTitle || 'Loaded trace'}`,
          subtab: route.page,
          focusProcess,
          engine: trace.engine,
        };
      }
      if (route.page === '/memscope') {
        return {
          mode: 'live',
          label: focusProcess ? `Live · ${focusProcess}` : 'Live device',
          subtab: 'Live',
          focusProcess,
        };
      }
      return {
        mode: 'none',
        label: 'No trace loaded',
        focusProcess,
      };
    },

    getSystemPrompt(ctx: AgentContextSnapshot): string {
      return [
        'You are the Memscope Assistant — an Android app memory specialist inside Perfetto Memscope.',
        'Your scope is what an app developer can reasonably diagnose inside a single process: Java heap dominators/retainers, class growth across dumps, smaps categories (Java, native, graphics, file, stack), and native heap profiles.',
        'Always call the structured `memscope_*` SQL tools before making numeric claims, and cite evidence IDs inline as `[E1]`, `[E2]`.',
        'Be concise, quantitative, and action-oriented. Do not fabricate numbers or classes that are not in tool output.',
        ctx.focusProcess
          ? `Currently focused process: ${ctx.focusProcess}.`
          : '',
      ]
        .filter(Boolean)
        .join('\n');
    },

    getSuggestions(ctx: AgentContextSnapshot): ReadonlyArray<SuggestedPrompt> {
      if (ctx.mode === 'trace') {
        return [
          {
            icon: 'summarize',
            label: 'Summarize captured memory profile',
            prompt:
              'Summarize what memory data is in this capture, which processes stand out, and where the memory went.',
          },
          {
            icon: 'account_tree',
            label: 'Top Java heap dominators',
            prompt:
              'What are the top retained Java classes and native allocations in the latest heap dump?',
          },
          {
            icon: 'layers',
            label: 'Break down smaps (Java / Native / Graphics)',
            prompt:
              'Break down the latest smaps snapshot by category (Java, Native, Graphics, File, Stack) and highlight what stands out.',
          },
          {
            icon: 'trending_up',
            label: 'Check for Java class growth / leaks',
            prompt:
              'If multiple Java heap dumps exist, which classes grew most between dumps? Otherwise explain the largest retained objects.',
          },
        ];
      }
      return [
        {
          icon: 'help_outline',
          label: 'How should I capture this app?',
          prompt:
            'Explain what Memscope captures (Java heap dumps, smaps, heapprofd) and how to spot a Java vs native vs graphics leak.',
        },
      ];
    },
  };
}

export const MemscopeAgentPlugin = createProductAgentPlugin(
  MEMSCOPE_AGENT_PLUGIN_ID,
  buildMemscopeAdapter,
);
