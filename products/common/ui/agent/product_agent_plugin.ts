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
import type {App} from '../../../../ui/src/public/app';
import type {
  PerfettoPlugin,
  PerfettoPluginStatic,
} from '../../../../ui/src/public/plugin';
import LlmPlugin from '../../../../ui/src/plugins/dev.perfetto.Llm';
import LlmProtocolOpenAiPlugin from '../../../../ui/src/plugins/dev.perfetto.LlmProtocolOpenAi';
import {ProductAgentSession} from './agent_loop';
import {AgentSidebar} from './agent_sidebar';
import {probeLocalOpenAiProvider} from './local_provider';
import type {ProductAgentAdapter} from './types';

export interface ProductAgentHandle {
  readonly uri: string;
  readonly session: ProductAgentSession;
  openAndAsk(prompt: string): void;
}

// Registers the shared Agentic Right Sidebar into Perfetto's native
// `app.sidePanel` (toggleable from the topbar, closed by default).
export function mountProductAgentSidebar(
  app: App,
  adapter: ProductAgentAdapter,
): ProductAgentHandle {
  const gateway = LlmPlugin.gateway;
  void probeLocalOpenAiProvider(gateway);

  const session = new ProductAgentSession(gateway, adapter);
  const uri = `perfetto.product.${adapter.productId}.assistant`;

  app.sidePanel.registerTab({
    uri,
    title: `${adapter.productName} Assistant`,
    icon: 'auto_awesome',
    render: () => m(AgentSidebar, {app, session}),
  });

  return {
    uri,
    session,
    openAndAsk(prompt: string) {
      app.sidePanel.showTab(uri);
      session.activeTab = 'chat';
      void session.send(prompt);
    },
  };
}

// Factory for products (like Memscope) that want to attach the shared Agentic
// Right Sidebar purely via `Embedder.extraPlugins`.
export function createProductAgentPlugin(
  id: string,
  createAdapter: (app: App) => ProductAgentAdapter,
): PerfettoPluginStatic<PerfettoPlugin> {
  return class ProductAgentPlugin implements PerfettoPlugin {
    static readonly id = id;
    static readonly description = `Agentic sidebar for ${id}`;
    static readonly dependencies = [LlmPlugin, LlmProtocolOpenAiPlugin];

    static onActivate(app: App): void {
      mountProductAgentSidebar(app, createAdapter(app));
    }
  };
}
