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

// The settings-page UI for the LLM gateway: configure providers (pick a
// protocol, fill in its credential fields, list the models to surface) and pick
// the default model. Rendered inline on the settings page via the custom
// `render` hook on the gateway's settings (see index.ts). The provider form is
// data-driven off each protocol's declared `credentialFields`, so the gateway
// never hard-codes any backend's login form.

import './settings.scss';
import m from 'mithril';
import {uuidv4} from '../../base/uuid';
import type {Setting} from '../../public/settings';
import {Button, ButtonVariant} from '../../widgets/button';
import {Combobox} from '../../widgets/combobox';
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';
import type {LlmGateway} from './gateway';
import {
  MODEL_ROLES,
  type Model,
  type ModelRole,
  type Provider,
  type ProvidersSetting,
} from './config';
import {Checkbox} from '../../widgets/checkbox';
import {assertIsInstance} from '../../base/assert';
import {AsyncMemo} from '../../base/async_memo';

type ModelFetchResult =
  | {readonly status: 'done'; readonly models: readonly string[]}
  // 'unsupported' = the protocol can't list models; fall back to free text with
  // no suggestions. 'error' = it tried and failed (bad key, unreachable, ...).
  | {readonly status: 'unsupported'}
  | {readonly status: 'error'; readonly message: string};

// Returns the current fetch state for a provider's available models, kicking off
// a background fetch (and a redraw on completion) if none is in flight for the
// current key.
async function fetchAvailableModels(
  gateway: LlmGateway,
  provider: Provider,
  signal?: AbortSignal,
): Promise<ModelFetchResult> {
  try {
    const models = await gateway.listAvailableModels(provider.id, signal);
    if (models === undefined) {
      return {status: 'unsupported'};
    } else {
      return {status: 'done', models: models.map((mm) => mm.name)};
    }
  } catch (e) {
    const message = e instanceof Error ? e.message : String(e);
    return {status: 'error', message};
  }
}

// Read-modify-write a provider list immutably. The setting persists on set().
function update(
  setting: Setting<ProvidersSetting>,
  fn: (providers: Provider[]) => Provider[],
): void {
  const next = fn([...setting.get()]);
  setting.set(next);
}

function patchProvider(
  setting: Setting<ProvidersSetting>,
  id: string,
  fn: (p: Provider) => Provider,
): void {
  update(setting, (providers) =>
    providers.map((p) => (p.id === id ? fn(p) : p)),
  );
}

// --- The providers editor ----------------------------------------------------

interface LlmSettingsAttrs {
  readonly gateway: LlmGateway;
  readonly setting: Setting<ProvidersSetting>;
}

export function LlmSettings(): m.Component<LlmSettingsAttrs> {
  return {
    view({attrs}: m.Vnode<LlmSettingsAttrs>) {
      const {gateway, setting} = attrs;
      const protocols = gateway.listProtocols();
      const providers = setting.get();

      return m(
        '.pf-llm-settings',
        providers.length === 0
          ? m('.pf-llm-settings__empty', 'No providers configured yet.')
          : providers.map((provider) =>
              m(ProviderEditor, {gateway, setting, provider}),
            ),
        protocols.length === 0
          ? m(
              '.pf-llm-settings__empty',
              'No LLM protocols are registered. Enable a protocol plugin (e.g. ' +
                'dev.perfetto.LlmProtocolGemini) to add providers.',
            )
          : m(Button, {
              label: 'Add provider',
              icon: 'add',
              variant: ButtonVariant.Filled,
              onclick: () => {
                // New providers default to the first registered protocol.
                const protocol = protocols[0];
                update(setting, (ps) => [
                  ...ps,
                  {
                    id: uuidv4(),
                    protocol: protocol.id,
                    credentials: {},
                    models: [],
                  },
                ]);
              },
            }),
      );
    },
  };
}

interface ProviderEditorAttrs {
  readonly gateway: LlmGateway;
  readonly setting: Setting<ProvidersSetting>;
  readonly provider: Provider;
}

function ProviderEditor(): m.Component<ProviderEditorAttrs> {
  const modelsSlot = new AsyncMemo<ModelFetchResult>();

  return {
    view({attrs}: m.Vnode<ProviderEditorAttrs>) {
      const {gateway, setting, provider} = attrs;
      const protocol = gateway.getProtocol(provider.protocol);

      const availableModels = modelsSlot.use({
        key: [provider.protocol, provider.credentials],
        // TODO: wire the QuerySlot's CancellationSignal through to
        // fetchAvailableModels' AbortSignal so in-flight fetches are actually
        // cancelled when the key changes.
        compute: () => fetchAvailableModels(gateway, provider),
      });

      let status: m.Children = null;
      let modelsToPick: readonly string[] = [];
      const models = availableModels.data;
      if (!models) {
        status = m('span.pf-llm-models-status', 'fetching available models…');
      } else {
        if (models.status === 'done') {
          status = m(
            'span.pf-llm-models-status',
            `${models.models.length} models available`,
          );
          modelsToPick = models.models;
        } else if (models.status === 'error') {
          status = m(
            'span.pf-llm-models-status.pf-llm-models-status--error',
            {title: models.message},
            'couldn’t fetch models — type the name manually',
          );
        } else if (models.status === 'unsupported') {
          status = m(
            'span.pf-llm-models-status',
            `This protocol doesn't support listing models`,
          );
        }
      }

      const modelStatusRow = m(
        'span.pf-llm-models-status-row',
        status,
        models?.status !== 'unsupported' &&
          m(Button, {
            icon: 'refresh',
            title: 'Refresh available models',
            onclick: () => {
              modelsSlot.invalidate();
            },
          }),
      );

      return m(
        '.pf-llm-provider',
        m(
          '.pf-llm-provider__header',
          m(TextInput, {
            value: provider.label,
            placeholder: 'Provider label',
            oninput: (e: Event) => {
              assertIsInstance(e.target, HTMLInputElement);
              const value = e.target.value;
              patchProvider(setting, provider.id, (p) => ({
                ...p,
                label: value,
              }));
            },
          }),
          m(
            Select,
            {
              value: provider.protocol,
              onchange: (e: Event) =>
                patchProvider(setting, provider.id, (p) => ({
                  ...p,
                  protocol: (e.target as HTMLSelectElement).value,
                  // Credentials are protocol-specific; clear on protocol change.
                  credentials: {},
                })),
            },
            gateway
              .listProtocols()
              .map((pr) => m('option', {value: pr.id}, pr.label)),
          ),
          m(Button, {
            icon: 'delete',
            title: 'Remove provider',
            onclick: () =>
              update(setting, (ps) => ps.filter((p) => p.id !== provider.id)),
          }),
        ),

        // Credential fields, driven by the protocol's declaration.
        protocol === undefined
          ? m(
              '.pf-llm-provider__warning',
              `Protocol "${provider.protocol}" is not registered. Enable its ` +
                'plugin or pick another protocol.',
            )
          : protocol.credentialFields.map((field) =>
              m('.pf-llm-field', [
                m('label.pf-llm-field__label', field.label),
                m(TextInput, {
                  value: provider.credentials[field.key] ?? '',
                  type: field.secret ? 'password' : 'text',
                  placeholder: field.placeholder ?? '',
                  oninput: (e: Event) =>
                    patchProvider(setting, provider.id, (p) => ({
                      ...p,
                      credentials: {
                        ...p.credentials,
                        [field.key]: (e.target as HTMLInputElement).value,
                      },
                    })),
                }),
              ]),
            ),

        // The model catalog.
        m('.pf-llm-provider__models-label', 'Models', modelStatusRow),
        provider.models.map((model) =>
          renderModelRow(setting, provider, model, modelsToPick),
        ),
        m(Button, {
          label: 'Add model',
          icon: 'add',
          onclick: () =>
            patchProvider(setting, provider.id, (p) => ({
              ...p,
              models: [
                ...p.models,
                {
                  id: uuidv4(),
                  modelName: '',
                  roles: ['agentic', 'flash'],
                },
              ],
            })),
        }),
      );
    },
  };
}

function renderModelRow(
  setting: Setting<ProvidersSetting>,
  provider: Provider,
  model: Model,
  availableModels: readonly string[],
): m.Children {
  const patchModel = (fn: (m: Model) => Model) =>
    patchProvider(setting, provider.id, (p) => ({
      ...p,
      models: p.models.map((m) => (m.id === model.id ? fn(m) : m)),
    }));

  const toggleRole = (role: ModelRole) =>
    patchModel((mdl) => ({
      ...mdl,
      roles: mdl.roles.includes(role)
        ? mdl.roles.filter((r) => r !== role)
        : [...mdl.roles, role],
    }));

  return m(
    '.pf-llm-model',
    m(TextInput, {
      value: model.label,
      placeholder: 'Display label',
      oninput: (e: Event) =>
        patchModel((mdl) => ({
          ...mdl,
          label: (e.target as HTMLInputElement).value,
        })),
    }),
    // The backend model name. A combobox: suggestions come from the provider's
    // listModels() when available, but the input stays free-text so a name the
    // backend didn't advertise (or any backend that can't list) still works.
    m(Combobox, {
      className: 'pf-llm-model__name',
      value: model.modelName,
      placeholder: 'Backend model name, e.g. gemini-2.5-flash',
      suggestions: availableModels,
      onChange: (value: string) =>
        patchModel((mdl) => ({...mdl, modelName: value})),
    }),
    m(
      '.pf-llm-model__roles',
      MODEL_ROLES.map((role) =>
        m(
          'label.pf-llm-model__role',
          m(Checkbox, {
            label: role,
            checked: model.roles.includes(role),
            onchange: () => toggleRole(role),
          }),
        ),
      ),
    ),
    m(Button, {
      icon: 'delete',
      title: 'Remove model',
      onclick: () =>
        patchProvider(setting, provider.id, (p) => ({
          ...p,
          models: p.models.filter((m) => m.id !== model.id),
        })),
    }),
  );
}
