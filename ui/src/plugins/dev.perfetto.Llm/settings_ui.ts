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
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';
import type {LlmGateway} from './gateway';
import {
  MODEL_ROLES,
  type Model,
  type ModelRole,
  type Provider,
  type ProvidersSetting,
  type SelectedModelRef,
} from './provider';

// Read-modify-write a provider list immutably. The setting persists on set().
function update(
  setting: Setting<ProvidersSetting>,
  fn: (providers: Provider[]) => Provider[],
): void {
  setting.set(fn([...setting.get()]));
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

export function renderProvidersSetting(
  gateway: LlmGateway,
  setting: Setting<ProvidersSetting>,
): m.Children {
  const protocols = gateway.listProtocols();
  const providers = setting.get();

  return m(
    '.pf-llm-settings',
    providers.length === 0
      ? m('.pf-llm-settings__empty', 'No providers configured yet.')
      : providers.map((p) => renderProvider(gateway, setting, p)),
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
                label: `${protocol.label} provider`,
                credentials: {},
                models: [],
              },
            ]);
          },
        }),
  );
}

function renderProvider(
  gateway: LlmGateway,
  setting: Setting<ProvidersSetting>,
  provider: Provider,
): m.Children {
  const protocol = gateway.getProtocol(provider.protocol);

  return m(
    '.pf-llm-provider',
    m(
      '.pf-llm-provider__header',
      m(TextInput, {
        value: provider.label,
        placeholder: 'Provider label',
        oninput: (e: Event) =>
          patchProvider(setting, provider.id, (p) => ({
            ...p,
            label: (e.target as HTMLInputElement).value,
          })),
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
    m('.pf-llm-provider__models-label', 'Models'),
    provider.models.map((model) =>
      renderModelRow(setting, provider, model),
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
              label: 'New model',
              modelName: '',
              roles: ['conversational'],
            },
          ],
        })),
    }),
  );
}

function renderModelRow(
  setting: Setting<ProvidersSetting>,
  provider: Provider,
  model: Model,
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
    m(TextInput, {
      value: model.modelName,
      placeholder: 'Backend model name, e.g. gemini-2.5-flash',
      oninput: (e: Event) =>
        patchModel((mdl) => ({
          ...mdl,
          modelName: (e.target as HTMLInputElement).value,
        })),
    }),
    m(
      '.pf-llm-model__roles',
      MODEL_ROLES.map((role) =>
        m(
          'label.pf-llm-model__role',
          m('input[type=checkbox]', {
            checked: model.roles.includes(role),
            onchange: () => toggleRole(role),
          }),
          role,
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

// --- The default-model picker ------------------------------------------------

export function renderDefaultModelSetting(
  gateway: LlmGateway,
  setting: Setting<SelectedModelRef>,
): m.Children {
  const models = gateway.listModels();
  if (models.length === 0) {
    return m(
      '.pf-llm-settings__empty',
      'Configure a provider with at least one model first.',
    );
  }

  const selected = setting.get();
  const value =
    selected === null ? '' : `${selected.providerId} ${selected.modelId}`;

  return m(
    Select,
    {
      value,
      onchange: (e: Event) => {
        const [providerId, modelId] = (
          e.target as HTMLSelectElement
        ).value.split(' ');
        setting.set({providerId, modelId});
      },
    },
    models.map((rm) =>
      m(
        'option',
        {value: `${rm.provider.id} ${rm.model.id}`},
        `${rm.provider.label} · ${rm.model.label}` +
          (rm.model.roles.length ? ` [${rm.model.roles.join(', ')}]` : ''),
      ),
    ),
  );
}
