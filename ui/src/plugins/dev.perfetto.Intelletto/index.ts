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

// dev.perfetto.Intelletto - the conversational assistant. Depends on the LLM
// gateway (dev.perfetto.Llm) for models and on a protocol plugin (e.g.
// dev.perfetto.LlmProtocolGemini) being enabled to actually talk to a backend.
// Renders a chat panel in the global side panel and wires up commands to open
// it.

import './styles.scss';
import m from 'mithril';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import LlmPlugin from '../dev.perfetto.Llm';
import {ChatPanel} from './chat_panel';

const SIDE_PANEL_URI = 'dev.perfetto.Intelletto#Chat';

export default class IntellettoPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Intelletto';
  static readonly description =
    'Conversational AI assistant. Ask about your trace in natural language; ' +
    'it queries the trace and drives the UI. Requires the dev.perfetto.Llm ' +
    'gateway and an LLM protocol plugin.';
  static readonly dependencies = [LlmPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    const gateway = LlmPlugin.gateway;

    // One chat panel per trace - the conversation is scoped to the open trace
    // and kept in memory only (re-created on the next trace load).
    trace.sidePanel.registerTab({
      uri: SIDE_PANEL_URI,
      title: 'Intelletto',
      icon: 'smart_toy',
      render: () => m(ChatPanel, {trace, gateway}),
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.Intelletto#Open',
      name: 'Intelletto: Open assistant',
      callback: () => trace.sidePanel.showTab(SIDE_PANEL_URI),
      defaultHotkey: 'Mod+Shift+A',
    });
  }
}
