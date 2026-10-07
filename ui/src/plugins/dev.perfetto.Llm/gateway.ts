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

// The LLM gateway: owns all three config layers (Protocol -> Provider ->
// Model) and exposes LLM capabilities to other plugins. This is the object the
// Intelletto assistant (and any future LLM consumer - summarisers, SQL
// autocomplete) talks to. It is deliberately app-scoped (not trace-scoped):
// configuration is independent of any loaded trace.

import type {Setting} from '../../public/settings';
import type {Request, Protocol, StreamEvent, AvailableModel} from './protocol';
import type {ModelRole, Provider, ProvidersSetting} from './config';

// Fully qualified path to a model containing providerId/modelId.
export interface ModelPath {
  readonly providerId: string;
  readonly modelId: string;
}

// Return result from listModels() - contains more in-depth information about
// the provider and the models.
export interface ModelDetails {
  readonly model: {
    readonly id: string;
    readonly label?: string;
    readonly modelName: string; // The name of the model at the protcol level
    readonly roles: readonly ModelRole[];
  };
  readonly provider: {
    readonly id: string;
    readonly label?: string;
    readonly protocolName: string;
  };
}

export class LlmGateway {
  // Protocols registered by plugins. Keyed by protocol id.
  private readonly protocols = new Map<string, Protocol>();

  // Providers pushed in at runtime rather than configured by the user: a
  // protocol plugin offering its on-device model with zero config, or a
  // connected extension server advertising its models. Held in memory only
  // (never persisted), and not editable/removable in settings - whoever pushed
  // them owns their lifetime. Keyed by id.
  private readonly staticProviders = new Map<string, Provider>();

  constructor(
    // User-supplied providers (persisted to settings).
    private readonly userProvidersSetting: Setting<ProvidersSetting>,
  ) {}

  // --- Protocol registry -----------------------------------------------------

  /**
   * Register a protocol (a backend integration, e.g. Gemini or an OpenAI-
   * compatible API). Typically called from a protocol plugin's onActivate.
   *
   * @param protocol The protocol to register.
   * @throws Error if a protocol with the same id is already registered.
   */
  registerProtocol(protocol: Protocol): void {
    if (this.protocols.has(protocol.id)) {
      throw new Error(`LLM protocol "${protocol.id}" already registered`);
    }
    this.protocols.set(protocol.id, protocol);
  }

  /**
   * Look up a registered protocol by id.
   *
   * @param id The protocol id.
   * @returns The protocol, or undefined if none is registered under that id.
   */
  getProtocol(id: string): Protocol | undefined {
    return this.protocols.get(id);
  }

  /**
   * List all registered protocols.
   *
   * @returns The registered protocols, in registration order.
   */
  listProtocols(): readonly Protocol[] {
    return Array.from(this.protocols.values());
  }

  // --- Providers and Models --------------------------------------------------

  /**
   * Push a provider supplied at runtime rather than by the user: a protocol
   * plugin's zero-config on-device model, or an extension server's advertised
   * models. Surfaced automatically alongside user providers but never persisted
   * and non-removable from settings. Typically called from a plugin's
   * onActivate (guard with a runtime feature-detection so an unusable backend
   * never gets registered), or when an extension server connects.
   *
   * @param provider The provider to register.
   * @throws Error if a provider with the same id is already registered.
   */
  registerProvider(provider: Provider): void {
    if (this.staticProviders.has(provider.id)) {
      throw new Error(`LLM provider "${provider.id}" already registered`);
    }
    this.staticProviders.set(provider.id, provider);
  }

  /**
   * The flat, concatenated provider list (user settings + runtime-pushed
   * providers). No precedence: a logical model appearing in more than one
   * source is just multiple entries.
   *
   * @returns All providers - user-configured first, then runtime-pushed.
   */
  listProviders(): readonly Provider[] {
    return [
      ...this.userProvidersSetting.get(),
      ...this.staticProviders.values(),
    ];
  }

  /**
   * Ask a provider's backend which models it can serve. The settings UI uses
   * this to populate the model-name combobox.
   *
   * @param providerId The id of the provider to query.
   * @param signal Optional abort signal to cancel the request.
   * @returns The available models, or undefined if the provider's protocol
   *   isn't registered or doesn't support listing.
   * @throws Rethrows network/auth errors so the caller can surface them.
   */
  async listAvailableModels(
    providerId: string,
    signal?: AbortSignal,
  ): Promise<readonly AvailableModel[] | undefined> {
    const provider = this.listProviders().find(({id}) => id === providerId);
    if (!provider) return undefined;
    const protocol = this.protocols.get(provider?.protocol);
    if (!protocol?.listModels) return undefined;
    return protocol.listModels(provider.credentials, signal);
  }

  // --- Model selection -------------------------------------------------------

  /**
   * Every (provider, model) pair across all providers - the flat list a picker
   * renders. Providers whose protocol isn't registered are skipped.
   *
   * @returns One entry per configured model, with its provider details.
   */
  listModels(): readonly ModelDetails[] {
    const out: ModelDetails[] = [];
    for (const provider of this.listProviders()) {
      // Look up the protocol in order to get the default protocol name
      const protocol = this.listProtocols().find(
        (p) => provider.protocol === p.id,
      );
      if (!protocol) continue;

      for (const model of provider.models) {
        out.push({
          provider: {...provider, protocolName: protocol.label},
          model,
        });
      }
    }
    return out;
  }

  // --- Running a turn --------------------------------------------------------

  /**
   * Start a new turn and stream back the result.
   *
   * This function is stateless - the full converstation history must be passed
   * in every time. This reflects the statelesss nature of the vast majority of
   * the backend APIs.
   *
   * @param model Fully qualified provider/model ids to use for this request.
   * @param request System prompt, previous messages, and tool definitions.
   * @param signal The abort signal called when we want to cancel the message.
   * @yields StreamEvents as the turn progresses (incremental text, thoughts,
   *   tool calls, usage, and a terminal stop event).
   */
  async *createStream(
    model: ModelPath,
    request: Request,
    signal: AbortSignal,
  ): AsyncGenerator<StreamEvent, void, void> {
    // Find the provider config
    const providers = this.listProviders();
    const provider = providers.find((p) => p.id === model.providerId);
    if (!provider) return undefined;

    // Find the model config in that provider config
    const modelInfo = provider.models.find((m) => m.id === model.modelId);
    if (!modelInfo) return undefined;

    // Look up the protocol listend in the provider config
    const protocol = this.protocols.get(provider.protocol);
    if (!protocol) return undefined;

    yield* protocol.createStream(
      modelInfo.modelName,
      request,
      provider.credentials,
      signal,
    );
  }
}
