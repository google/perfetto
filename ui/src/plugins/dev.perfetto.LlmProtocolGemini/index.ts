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

import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import LlmPlugin from '../dev.perfetto.Llm';
import {GeminiProtocol} from './gemini_protocol';

export default class LlmProtocolGeminiPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.LlmProtocolGemini';
  static readonly description =
    "Registers the 'gemini' LLM protocol (Google Gemini wire format) with " +
    'the dev.perfetto.Llm gateway.';
  static readonly dependencies = [LlmPlugin];

  static onActivate(_app: App): void {
    LlmPlugin.gateway.registerProtocol(new GeminiProtocol());
  }
}
