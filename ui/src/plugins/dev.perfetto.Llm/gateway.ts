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
import type {
  AvailableModel,
  NeutralRequest,
  Protocol,
  StreamEvent,
} from './protocol';
import type {
  Model,
  ModelRole,
  Provider,
  ProvidersSetting,
  ResolvedModel,
  SelectedModelRef,
} from './provider';

// What a consumer needs to actually drive a turn: the resolved model plus the
// protocol that knows how to talk to it. Returned by the model-lookup methods.
export interface LlmModelHandle {
  readonly resolved: ResolvedModel;
  readonly protocol: Protocol;
  // Run one turn. The request's model params are filled in from the resolved
  // model; the consumer supplies messages, system prompt and tools. Errors are
  // normalised into a terminal `stop` event by the protocol rather than thrown.
  createStream(
    request: Omit<NeutralRequest, 'params'> &
      Partial<Pick<NeutralRequest, 'params'>>,
    signal?: AbortSignal,
  ): AsyncGenerator<StreamEvent, void, void>;
}

export class LlmGateway {
  // Protocols registered by plugins. Keyed by protocol id.
  private readonly protocols = new Map<string, Protocol>();

  // Extension-server-supplied providers, held in memory only (never persisted).
  // Phase 1 has no extension-server wiring yet, but the gateway carries them so
  // the lookup path is identical for both sources.
  private extensionProviders: ReadonlyArray<Provider> = [];

  constructor(
    // User-supplied providers (persisted to settings).
    private readonly providersSetting: Setting<ProvidersSetting>,
    // The active Provider:Model pointer (persisted to settings).
    private readonly selectedModelSetting: Setting<SelectedModelRef>,
  ) {}

  // --- Protocol registry -----------------------------------------------------

  registerProtocol(protocol: Protocol): void {
    if (this.protocols.has(protocol.id)) {
      throw new Error(`LLM protocol "${protocol.id}" already registered`);
    }
    this.protocols.set(protocol.id, protocol);
  }

  getProtocol(id: string): Protocol | undefined {
    return this.protocols.get(id);
  }

  listProtocols(): ReadonlyArray<Protocol> {
    return Array.from(this.protocols.values());
  }

  // --- Providers -------------------------------------------------------------

  // Extension servers push their providers here on connect. In-memory only.
  setExtensionProviders(providers: ReadonlyArray<Provider>): void {
    this.extensionProviders = providers;
  }

  // The flat, concatenated provider list (user settings + extension server).
  // No precedence: a logical model appearing in both is just two entries.
  listProviders(): ReadonlyArray<Provider> {
    return [...this.providersSetting.get(), ...this.extensionProviders];
  }

  // Ask a provider's backend which models it can serve. Returns undefined if
  // the provider's protocol isn't registered or doesn't support listing;
  // rethrows network/auth errors so the caller can surface them. The settings
  // UI uses this to populate the model-name combobox.
  async listAvailableModels(
    provider: Provider,
    signal?: AbortSignal,
  ): Promise<ReadonlyArray<AvailableModel> | undefined> {
    const protocol = this.protocols.get(provider.protocol);
    if (protocol?.listModels === undefined) return undefined;
    return protocol.listModels(provider.credentials, signal);
  }

  private sourceOf(providerId: string): 'user' | 'extension-server' {
    return this.extensionProviders.some((p) => p.id === providerId)
      ? 'extension-server'
      : 'user';
  }

  // --- Model selection -------------------------------------------------------

  // The user's active Provider:Model pointer, resolved. null if nothing valid
  // is selected (no providers configured, or the pointer is stale).
  getSelectedModel(): ResolvedModel | undefined {
    const ref = this.selectedModelSetting.get();
    if (ref === null) return undefined;
    return this.resolveRef(ref);
  }

  setSelectedModel(providerId: string, modelId: string): void {
    this.selectedModelSetting.set({providerId, modelId});
  }

  private resolveRef(ref: SelectedModelRef): ResolvedModel | undefined {
    if (ref === null) return undefined;
    const provider = this.listProviders().find((p) => p.id === ref.providerId);
    if (provider === undefined) return undefined;
    const model = provider.models.find((m) => m.id === ref.modelId);
    if (model === undefined) return undefined;
    return {provider, model, source: this.sourceOf(provider.id)};
  }

  // Every (provider, model) pair across all providers - the flat list a picker
  // renders.
  listModels(): ReadonlyArray<ResolvedModel> {
    const out: ResolvedModel[] = [];
    for (const provider of this.listProviders()) {
      const source = this.sourceOf(provider.id);
      for (const model of provider.models) {
        out.push({provider, model, source});
      }
    }
    return out;
  }

  // The default model for a role: the selected model if it has the role,
  // otherwise the first model offering that role. This is what non-assistant
  // consumers call ("give me the default conversational/flash model") instead
  // of naming a specific model.
  getDefaultModel(role: ModelRole): ResolvedModel | undefined {
    const selected = this.getSelectedModel();
    if (selected !== undefined && hasRole(selected.model, role)) {
      return selected;
    }
    return this.listModels().find((rm) => hasRole(rm.model, role));
  }

  // --- Running a turn --------------------------------------------------------

  // Wrap a resolved model in a handle that knows how to stream a turn. Returns
  // undefined if no protocol is registered for the provider's protocol id (e.g.
  // the protocol plugin is disabled).
  getModelHandle(resolved: ResolvedModel): LlmModelHandle | undefined {
    const protocol = this.protocols.get(resolved.provider.protocol);
    if (protocol === undefined) return undefined;
    return makeHandle(resolved, protocol);
  }

  // Convenience: the handle for the default model of a role, or undefined if
  // none is configured / its protocol isn't registered.
  getDefaultModelHandle(role: ModelRole): LlmModelHandle | undefined {
    const resolved = this.getDefaultModel(role);
    if (resolved === undefined) return undefined;
    return this.getModelHandle(resolved);
  }
}

function hasRole(model: Model, role: ModelRole): boolean {
  return model.roles.includes(role);
}

function makeHandle(
  resolved: ResolvedModel,
  protocol: Protocol,
): LlmModelHandle {
  const {provider, model} = resolved;
  return {
    resolved,
    protocol,
    async *createStream(request, signal) {
      const params = request.params ?? {
        modelName: model.modelName,
        temperature: model.temperature,
        maxOutputTokens: model.maxOutputTokens,
      };
      const systemPrompt = [model.systemPrompt, request.systemPrompt]
        .filter((s): s is string => Boolean(s))
        .join('\n\n');
      yield* protocol.createStream(
        {...request, params, systemPrompt},
        provider.credentials,
        signal,
      );
    },
  };
}
