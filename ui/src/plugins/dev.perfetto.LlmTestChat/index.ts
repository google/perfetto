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

// dev.perfetto.LlmTestChat - a minimal, tool-free chat page for exercising the
// LLM gateway on its own, without the Intelletto assistant on top. Dev-only and
// off by default: it registers a page (#!/llm_chat) only when the
// 'llmTestChatPage' flag is enabled, so it costs nothing otherwise.

import m from 'mithril';
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import LlmPlugin from '../dev.perfetto.Llm';
import {LlmTestChatPage} from './test_chat_page';

export default class LlmTestChatPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.LlmTestChat';
  static readonly description =
    'Dev-only chat page (#!/llm_chat) that talks directly to the ' +
    'dev.perfetto.Llm gateway, for testing providers without the assistant. ' +
    'Enable the "llmTestChatPage" flag to show it.';
  static readonly dependencies = [LlmPlugin];

  static onActivate(app: App): void {
    const gateway = LlmPlugin.gateway;
    app.pages.registerPage({
      route: '/llm_chat',
      render: () => m(LlmTestChatPage, {gateway}),
    });
    app.sidebar.addMenuItem({
      section: 'support',
      text: 'LLM test chat',
      href: '#!/llm_chat',
      icon: 'forum',
    });
  }
}
