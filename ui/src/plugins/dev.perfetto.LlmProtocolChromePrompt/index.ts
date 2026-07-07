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

// dev.perfetto.LlmProtocolChromePrompt - registers the 'chrome-prompt' protocol
// with the common LLM gateway. This protocol talks to Chrome's experimental
// built-in Prompt API (the `LanguageModel` global), an on-device Gemma-based
// model that runs entirely in the browser with no network or API key. Add a
// provider pointing at this protocol to use it. Tool calling is emulated, so it
// works best for plain chat / light tool use.

import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import LlmPlugin from '../dev.perfetto.Llm';
import {
  ChromePromptProtocol,
  CHROME_PROMPT_BUILTIN_PROVIDER,
  isChromePromptApiPresent,
} from './chrome_prompt_protocol';

export default class LlmProtocolChromePromptPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.LlmProtocolChromePrompt';
  static readonly description =
    "Registers the 'chrome-prompt' LLM protocol (Chrome's built-in on-device " +
    'Prompt API / Gemini Nano) with the dev.perfetto.Llm gateway. Runs ' +
    'locally in the browser - no network or API key.';
  static readonly dependencies = [LlmPlugin];

  static onActivate(_app: App): void {
    LlmPlugin.gateway.registerProtocol(new ChromePromptProtocol());
    // Zero-config: if this browser actually exposes the Prompt API, push a
    // ready-to-use provider so the assistant has a working on-device model with
    // no key and no settings. Gated on the API being present so we don't offer a
    // model that can't run.
    if (isChromePromptApiPresent()) {
      LlmPlugin.gateway.registerProvider(CHROME_PROMPT_BUILTIN_PROVIDER);
    }
  }
}
