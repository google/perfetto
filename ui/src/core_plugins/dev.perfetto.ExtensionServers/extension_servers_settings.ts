// Copyright (C) 2024 The Android Open Source Project
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

import m from 'mithril';
import {AppImpl} from '../../core/app_impl';
import {Button} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';
import {ExtensionServer} from './types';
import {showAddExtensionServerModal} from './add_extension_server_modal';

export function renderExtensionServersSettings(): m.Children {
  const app = AppImpl.instance;
  const setting = app.settings.get('extensionServers');
  if (!setting) return null;

  const servers = (setting.get() ?? []) as ExtensionServer[];

  const addServer = async () => {
    const result = await showAddExtensionServerModal();
    if (result) {
      const newServers = [...servers, result];
      setting.set(newServers);
    }
  };

  const editServer = async (index: number) => {
    const result = await showAddExtensionServerModal(servers[index]);
    if (result) {
      const newServers = [...servers];
      newServers[index] = result;
      setting.set(newServers);
    }
  };

  const deleteServer = (index: number) => {
    const newServers = servers.filter((_, i) => i !== index);
    setting.set(newServers);
  };

  const toggleEnabled = (index: number) => {
    const newServers = [...servers];
    newServers[index] = {
      ...newServers[index],
      enabled: !newServers[index].enabled,
    };
    setting.set(newServers);
  };

  return m(
    '.pf-extension-servers-settings',
    servers.length === 0
      ? m(
          '.pf-extension-servers-settings__empty',
          m(
            EmptyState,
            {
              title: 'No extension servers configured',
              icon: 'dns',
            },
            m(Button, {
              label: 'Add Server',
              icon: 'add',
              onclick: addServer,
            }),
          ),
        )
      : [
          m(
            '.pf-extension-servers-settings__header',
            m(Button, {
              label: 'Add Server',
              icon: 'add',
              onclick: addServer,
            }),
          ),
          m(
            '.pf-extension-servers-settings__list',
            servers.map((server, idx) =>
              m(
                '.pf-extension-servers-settings__item',
                {key: idx},
                m(
                  '.pf-extension-servers-settings__item-content',
                  m(
                    '.pf-extension-servers-settings__item-url',
                    server.url
                      .replace(/^github:\/\//, '')
                      .replace(/^https?:\/\//, ''),
                  ),
                  m(
                    '.pf-extension-servers-settings__item-info',
                    `${server.selectedModules.length} module${server.selectedModules.length === 1 ? '' : 's'}`,
                    !server.enabled && ' (Disabled)',
                  ),
                ),
                m(
                  '.pf-extension-servers-settings__item-actions',
                  m(Button, {
                    icon: server.enabled ? 'toggle_on' : 'toggle_off',
                    title: server.enabled ? 'Disable' : 'Enable',
                    compact: true,
                    onclick: () => toggleEnabled(idx),
                  }),
                  m(Button, {
                    icon: 'edit',
                    title: 'Edit',
                    compact: true,
                    onclick: () => editServer(idx),
                  }),
                  m(Button, {
                    icon: 'delete',
                    title: 'Delete',
                    compact: true,
                    onclick: () => deleteServer(idx),
                  }),
                ),
              ),
            ),
          ),
        ],
  );
}
