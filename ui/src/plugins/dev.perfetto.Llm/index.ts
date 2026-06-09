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

// dev.perfetto.Llm - the common LLM gateway plugin. It owns the
// Protocol/Provider/Model config and exposes an LlmGateway to other plugins
// (the Intelletto assistant, and any future LLM consumer). Protocol plugins
// register their protocol against the gateway; consumer plugins ask it for a
// model handle to run a turn.

import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Setting} from '../../public/settings';
import {assertExists} from '../../base/assert';
import {LlmGateway} from './gateway';
import {
  PROVIDERS_SETTING_SCHEMA,
  type ProvidersSetting,
  SELECTED_MODEL_SCHEMA,
  type SelectedModelRef,
} from './provider';
import {
  renderDefaultModelSetting,
  renderProvidersSetting,
} from './settings_ui';

export default class LlmPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Llm';
  static readonly description =
    'Common LLM gateway. Owns Protocol/Provider/Model configuration and ' +
    'exposes LLM capabilities to other plugins (e.g. the Intelletto ' +
    'assistant). Protocol plugins register against it; consumers ask it for ' +
    'a model to run.';

  // The gateway is app-scoped: configuration is independent of any loaded
  // trace, and protocol plugins register in onActivate (also pre-trace). We
  // expose it via a static accessor so dependent plugins can reach it without
  // a trace, mirroring how the gateway's own settings are app-level.
  private static gatewayInstance?: LlmGateway;

  private static providersSetting: Setting<ProvidersSetting>;
  private static selectedModelSetting: Setting<SelectedModelRef>;

  static onActivate(app: App): void {
    // User-supplied providers. Persisted to local storage via settings.
    // Credentials (API keys) are stored in plaintext - a conscious choice; see
    // the RFC's credential-handling notes (the browser offers no storage that
    // resists XSS, and it's the user's own key on their own machine). This is
    // configurable directly from the settings page via the custom renderer
    // below, which is driven off each protocol's declared credential fields.
    LlmPlugin.providersSetting = app.settings.register({
      id: `${LlmPlugin.id}#Providers`,
      name: 'LLM providers',
      description:
        'LLM providers available to the assistant and other LLM features. ' +
        'Each provider picks a protocol (e.g. Gemini), supplies its ' +
        'credentials, and lists the models it offers.',
      schema: PROVIDERS_SETTING_SCHEMA,
      defaultValue: [],
      // The renderer reads LlmPlugin.gateway at draw time (long after
      // onActivate), so the gateway being constructed below is available.
      render: (setting) => renderProvidersSetting(LlmPlugin.gateway, setting),
    });

    // The active Provider:Model pointer - the default model consumers run
    // against. Configurable from the settings page (and also from the chat
    // header for in-the-moment switching).
    LlmPlugin.selectedModelSetting = app.settings.register({
      id: `${LlmPlugin.id}#SelectedModel`,
      name: 'Default LLM model',
      description:
        'The model used by default by the assistant and other LLM features.',
      schema: SELECTED_MODEL_SCHEMA,
      defaultValue: null,
      render: (setting) =>
        renderDefaultModelSetting(LlmPlugin.gateway, setting),
    });

    LlmPlugin.gatewayInstance = new LlmGateway(
      LlmPlugin.providersSetting,
      LlmPlugin.selectedModelSetting,
    );
  }

  // Accessor for dependent plugins (protocol providers and consumers). Throws
  // if called before onActivate, which can't happen for a plugin that declares
  // a dependency on this one (dependencies activate first).
  static get gateway(): LlmGateway {
    return assertExists(
      LlmPlugin.gatewayInstance,
      'LLM gateway accessed before dev.perfetto.Llm was activated',
    );
  }
}
