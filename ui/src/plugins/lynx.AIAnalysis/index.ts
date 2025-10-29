// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {App} from '../../public/app';
import {Setting} from '../../public/settings';
import {z} from 'zod';

export default class AIAnalysis implements PerfettoPlugin {
  static readonly id = 'lynx.AIAnalysis';
  static APIKeySetting: Setting<string>;
  static baseUrlSetting: Setting<string>;
  static modelNameSetting: Setting<string>;
  static modelProviderSetting: Setting<string>;
  static customPromptSetting: Setting<string>;

  static onActivate(app: App): void {
    AIAnalysis.modelNameSetting = app.settings.register({
      id: `${AIAnalysis.id}#ModelNameSetting`,
      name: 'LLM Model',
      description:
        'The specific LLM model to use, such as seed-1.5, seed-1.6 for doubao provider.',
      schema: z.string(),
      defaultValue: '',
      requiresReload: true,
    });
    AIAnalysis.modelProviderSetting = app.settings.register({
      id: `${AIAnalysis.id}#ModelProviderSetting`,
      name: 'LLM Model Provider',
      description:
        'The LLM model provider to use, such as doubao, openai, anthropic, google_gemini.',
      schema: z.string(),
      defaultValue: '',
      requiresReload: true,
    });
    AIAnalysis.APIKeySetting = app.settings.register({
      id: `${AIAnalysis.id}#APIKeySetting`,
      name: 'LLM API Key',
      description: 'API key for your chosen provider.',
      schema: z.string(),
      defaultValue: '',
      requiresReload: true,
    });
    AIAnalysis.baseUrlSetting = app.settings.register({
      id: `${AIAnalysis.id}#BaseUrlSetting`,
      name: 'Base URL',
      description:
        'The LLM base URL(optinal), such as https://ark.cn-beijing.volces.com/api/v3.',
      schema: z.string(),
      defaultValue: '',
      requiresReload: true,
    });
    AIAnalysis.customPromptSetting = app.settings.register({
      id: `${AIAnalysis.id}#CustomPromptSetting`,
      name: 'Custom Prompt',
      description: 'Custom prompt for LLM analysis.',
      schema: z.string(),
      defaultValue: '',
      requiresReload: true,
    });
  }

  async onTraceLoad(_trace: Trace): Promise<void> {}
}
