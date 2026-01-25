// Copyright (C) 2025 The Android Open Source Project
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
import {App} from '../../public/app';
import {
  LanguageModelProvider,
  LanguageModelStatus,
  GenerateOptions,
} from '../../public/language_model';
import {PerfettoPlugin} from '../../public/plugin';
import {Setting} from '../../public/settings';

/**
 * Claude provider using the Anthropic API with SSE streaming.
 */
class ClaudeProvider implements LanguageModelProvider {
  private readonly apiKeySetting: Setting<string>;
  private readonly modelSetting: Setting<string>;
  private readonly maxTokensSetting: Setting<number>;

  readonly info = {
    id: 'claude',
    name: 'Claude',
    requiresApiKey: true,
    description:
      'Uses the Anthropic Claude API. Requires an API key from console.anthropic.com.',
  };

  constructor(
    apiKeySetting: Setting<string>,
    modelSetting: Setting<string>,
    maxTokensSetting: Setting<number>,
  ) {
    this.apiKeySetting = apiKeySetting;
    this.modelSetting = modelSetting;
    this.maxTokensSetting = maxTokensSetting;
  }

  async checkStatus(): Promise<LanguageModelStatus> {
    const apiKey = this.apiKeySetting.get();
    if (!apiKey) {
      return {
        status: 'unavailable',
        message: 'Claude API key is required. Please set it in Settings.',
      };
    }
    return {status: 'available'};
  }

  async generate(options: GenerateOptions): Promise<string> {
    const apiKey = this.apiKeySetting.get();
    const model = this.modelSetting.get() || 'claude-sonnet-4-5';
    const maxTokens = this.maxTokensSetting.get() || 8192;

    if (!apiKey) {
      throw new Error('Claude API key is required. Please set it in Settings.');
    }

    const response = await fetch('https://api.anthropic.com/v1/messages', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'x-api-key': apiKey,
        'anthropic-version': '2023-06-01',
        'anthropic-dangerous-direct-browser-access': 'true',
      },
      body: JSON.stringify({
        model: model,
        max_tokens: maxTokens,
        stream: true,
        system: options.systemPrompt,
        messages: [{role: 'user', content: options.userPrompt}],
      }),
    });

    if (!response.ok) {
      const error = await response.text();
      throw new Error(`Claude API error: ${error}`);
    }

    if (!response.body) {
      throw new Error('No response body from Claude API');
    }

    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    let fullText = '';
    let buffer = '';

    while (true) {
      const {done, value} = await reader.read();
      if (done) break;

      buffer += decoder.decode(value, {stream: true});
      const lines = buffer.split('\n');
      buffer = lines.pop() || '';

      for (const line of lines) {
        if (line.startsWith('data: ')) {
          const jsonStr = line.slice(6);
          if (jsonStr === '[DONE]') continue;

          try {
            const event = JSON.parse(jsonStr);
            if (
              event.type === 'content_block_delta' &&
              event.delta?.text !== undefined
            ) {
              fullText += event.delta.text;
              if (options.onProgress) {
                options.onProgress(fullText);
              }
            }
          } catch {
            // Skip invalid JSON lines
          }
        }
      }
    }

    if (!fullText) {
      throw new Error('No response from Claude API');
    }

    return fullText;
  }
}

/**
 * Plugin that provides the Claude language model provider.
 */
export default class ClaudePlugin implements PerfettoPlugin {
  static readonly id = 'com.anthropic.Claude';

  static onActivate(app: App): void {
    // Register Claude settings under com.anthropic.Claude section
    const apiKeySetting = app.settings.register({
      id: `${ClaudePlugin.id}#apiKey`,
      name: 'API Key',
      description:
        'Your Anthropic API key for Claude. Get one at console.anthropic.com.',
      schema: z.string(),
      defaultValue: '',
    });

    const modelSetting = app.settings.register({
      id: `${ClaudePlugin.id}#model`,
      name: 'Model',
      description: 'Select which Claude model to use.',
      schema: z.string(),
      defaultValue: 'claude-sonnet-4-5',
    });

    const maxTokensSetting = app.settings.register({
      id: `${ClaudePlugin.id}#maxTokens`,
      name: 'Max Output Tokens',
      description:
        'Maximum number of tokens in Claude responses. Higher values allow ' +
        'longer responses but may increase costs.',
      schema: z.number().int().min(1).max(64000),
      defaultValue: 16000,
    });

    // Register the Claude provider
    const provider = new ClaudeProvider(
      apiKeySetting,
      modelSetting,
      maxTokensSetting,
    );
    app.languageModels.registerProvider(provider);
  }
}
