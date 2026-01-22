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

// Chrome LanguageModel API types
interface DownloadProgressEvent extends Event {
  loaded?: number;
  total?: number;
}

interface CreateMonitor extends EventTarget {}

type Availability =
  | 'unavailable'
  | 'downloadable'
  | 'downloading'
  | 'available';

interface LanguageModelApi {
  availability(options: object): Promise<Availability>;
  create(options: {
    expectedInputs?: Array<{type: string; languages: string[]}>;
    expectedOutputs?: Array<{type: string; languages: string[]}>;
    initialPrompts?: Array<{role: string; content: string}>;
    temperature?: number;
    topK?: number;
    monitor?: (monitor: CreateMonitor) => void;
  }): Promise<LanguageModelSession>;
}

interface LanguageModelSession {
  prompt(input: string): Promise<string>;
  promptStreaming(input: string): ReadableStream<string>;
  destroy(): void;
}

declare const LanguageModel: LanguageModelApi;

/**
 * Gemini Nano provider using Chrome's built-in LanguageModel API.
 */
class GeminiNanoProvider implements LanguageModelProvider {
  private readonly temperatureSetting: Setting<number>;
  private readonly topKSetting: Setting<number>;

  readonly info = {
    id: 'gemini-nano',
    name: 'Gemini Nano',
    requiresApiKey: false,
    description:
      "Uses Chrome's built-in Gemini Nano model. Requires Chrome 127+ with specific flags enabled.",
  };

  constructor(
    temperatureSetting: Setting<number>,
    topKSetting: Setting<number>,
  ) {
    this.temperatureSetting = temperatureSetting;
    this.topKSetting = topKSetting;
  }

  async checkStatus(): Promise<LanguageModelStatus> {
    if (!('LanguageModel' in self)) {
      return {
        status: 'not-supported',
        message:
          'Gemini Nano is not available in this browser. To enable it, ' +
          '1. Use Chrome 127 or newer, ' +
          '2. Go to chrome://flags/#optimization-guide-on-device-model and set to "Enabled BypassPerfRequirement", ' +
          '3. Go to chrome://flags/#prompt-api-for-gemini-nano and set to "Enabled", ' +
          '4. Restart Chrome. ' +
          'Alternatively, select a different language model provider (e.g. Claude).',
      };
    }

    try {
      const availability: Availability = await LanguageModel.availability({});
      if (availability === 'unavailable') {
        return {
          status: 'unavailable',
          message:
            'Gemini Nano model is not available. The model may not be supported on this device.',
        };
      }
      if (availability === 'downloadable') {
        return {status: 'downloadable'};
      }
      if (availability === 'downloading') {
        return {status: 'downloading', progress: 0};
      }
      return {status: 'available'};
    } catch (e) {
      return {
        status: 'unavailable',
        message: `Error checking availability: ${e instanceof Error ? e.message : String(e)}`,
      };
    }
  }

  async downloadModel(onProgress: (progress: number) => void): Promise<void> {
    if (!('LanguageModel' in self)) {
      throw new Error('LanguageModel API is not available');
    }

    const downloadPromise = new Promise<void>((resolve, reject) => {
      LanguageModel.create({
        monitor(monitor: CreateMonitor) {
          monitor.addEventListener('downloadprogress', (e: Event) => {
            const progressEvent = e as DownloadProgressEvent;
            const loaded = progressEvent.loaded ?? 0;
            const total = progressEvent.total ?? 1;
            const progress = total > 0 ? (loaded / total) * 100 : 0;
            onProgress(progress);
          });
        },
        expectedInputs: [{type: 'text', languages: ['en']}],
        expectedOutputs: [{type: 'text', languages: ['en']}],
      })
        .then((session) => {
          session.destroy();
          resolve();
        })
        .catch(reject);
    });

    await downloadPromise;
  }

  async generate(options: GenerateOptions): Promise<string> {
    const status = await this.checkStatus();

    if (status.status === 'not-supported') {
      throw new Error(status.message);
    }
    if (status.status === 'unavailable') {
      throw new Error(status.message);
    }
    if (status.status === 'downloadable' || status.status === 'downloading') {
      throw new Error('Gemini Nano model needs to be downloaded first');
    }

    // Get settings from registered settings
    const temperature = this.temperatureSetting.get();
    const topK = this.topKSetting.get();

    try {
      const session = await LanguageModel.create({
        expectedInputs: [{type: 'text', languages: ['en']}],
        expectedOutputs: [{type: 'text', languages: ['en']}],
        initialPrompts: [{role: 'system', content: options.systemPrompt}],
        temperature,
        topK,
      });

      const stream = session.promptStreaming(options.userPrompt);
      const reader = stream.getReader();
      let response = '';

      while (true) {
        const {done, value} = await reader.read();
        if (done) break;
        response += value;
        if (options.onProgress) {
          options.onProgress(response);
        }
      }

      session.destroy();

      return response;
    } catch (e) {
      const errorMessage = e instanceof Error ? e.message : String(e);
      if (errorMessage.includes('The input is too large')) {
        throw new Error(
          `Gemini Nano error: ${errorMessage} Try reducing the size of the input.`,
        );
      }
      throw new Error(`Gemini Nano error: ${errorMessage}`);
    }
  }
}

/**
 * Plugin that provides the Gemini Nano language model provider.
 */
export default class GeminiNanoPlugin implements PerfettoPlugin {
  static readonly id = 'com.google.GeminiNano';

  static onActivate(app: App): void {
    // Register Gemini Nano settings under com.google.GeminiNano section
    const temperatureSetting = app.settings.register({
      id: `${GeminiNanoPlugin.id}#temperature`,
      name: 'Temperature',
      description:
        'Controls randomness in Gemini Nano responses. Higher values (up to 2.0) ' +
        'make output more random, lower values make it more deterministic.',
      schema: z.number().min(0).max(2),
      defaultValue: 1.0,
    });

    const topKSetting = app.settings.register({
      id: `${GeminiNanoPlugin.id}#topK`,
      name: 'Top-K',
      description:
        'Limits the number of tokens considered for each step in Gemini Nano. ' +
        'Lower values make output more focused.',
      schema: z.number().int().min(1),
      defaultValue: 3,
    });

    // Register the Gemini Nano provider
    const provider = new GeminiNanoProvider(temperatureSetting, topKSetting);
    app.languageModels.registerProvider(provider);
  }
}
