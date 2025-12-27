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
import {Button} from '../../widgets/button';
import {showModal} from '../../widgets/modal';
import {TextInput} from '../../widgets/text_input';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {FormLabel} from '../../widgets/form';
import {Card} from '../../widgets/card';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {Tooltip} from '../../widgets/tooltip';
import {Icon} from '../../widgets/icon';
import {MultiSelect, MultiSelectDiff} from '../../widgets/multiselect';
import {ExtensionServer} from './types';
import {fetchManifest} from './extension_server';
import {resolveServerUrl} from './url_utils';

type ServerType = 'github' | 'http';

interface AddServerModalState {
  serverType: ServerType;
  // GitHub fields
  githubRepo: string; // Format: owner/repo
  githubRef: string;
  // HTTP field
  httpUrl: string;
  loadingManifest: boolean;
  manifestError?: string;
  availableModules: string[];
  selectedModules: Set<string>;
}

class AddExtensionServerModal {
  state: AddServerModalState;
  private editingServer: ExtensionServer | null;

  constructor(server?: ExtensionServer) {
    this.editingServer = server ?? null;
    this.state = this.createInitialState(server);
  }

  private createInitialState(server?: ExtensionServer): AddServerModalState {
    const baseState = {
      loadingManifest: false,
      availableModules: [],
      selectedModules: new Set(server?.selectedModules ?? []),
    };

    if (!server) {
      return {
        ...baseState,
        serverType: 'github',
        githubRepo: '',
        githubRef: 'main',
        httpUrl: '',
      };
    }

    const isGithub = server.url.startsWith('github://');
    if (isGithub) {
      const match = server.url.match(/^github:\/\/([^/]+\/[^/]+)\/(.+)$/);
      return {
        ...baseState,
        serverType: 'github',
        githubRepo: match?.[1] ?? '',
        githubRef: match?.[2] ?? 'main',
        httpUrl: '',
      };
    }

    return {
      ...baseState,
      serverType: 'http',
      githubRepo: '',
      githubRef: 'main',
      httpUrl: server.url,
    };
  }

  private getCurrentUrl(): string {
    if (this.state.serverType === 'github') {
      const {githubRepo, githubRef} = this.state;
      return `github://${githubRepo}/${githubRef}`;
    } else {
      return this.state.httpUrl;
    }
  }

  private isUrlValid(): boolean {
    if (this.state.serverType === 'github') {
      const parts = this.state.githubRepo.trim().split('/');
      return !!(
        parts.length === 2 &&
        parts[0] &&
        parts[1] &&
        this.state.githubRef.trim()
      );
    } else {
      return !!this.state.httpUrl.trim();
    }
  }

  private async loadManifest(): Promise<void> {
    if (!this.isUrlValid()) return;

    this.state.loadingManifest = true;
    this.state.manifestError = undefined;
    m.redraw();

    try {
      const url = this.getCurrentUrl();
      const resolvedUrl = resolveServerUrl(url);
      const manifest = await fetchManifest(resolvedUrl);

      if (!manifest) {
        const location =
          this.state.serverType === 'github'
            ? `${this.state.githubRepo} @ ${this.state.githubRef}`
            : this.state.httpUrl;
        const hint =
          this.state.serverType === 'github'
            ? 'Please check that the repository, branch, and manifest.json file exist.'
            : 'Check that the URL is correct and CORS is enabled.';
        this.state.manifestError = `Could not fetch manifest from ${location}. ${hint}`;
        this.state.availableModules = [];
      } else {
        this.state.availableModules = manifest.modules;
        // Keep existing selections if editing, otherwise select 'default' if available
        if (!this.editingServer) {
          this.state.selectedModules = new Set();
          if (manifest.modules.includes('default')) {
            this.state.selectedModules.add('default');
          }
        }
      }
    } catch (e) {
      this.state.manifestError = `Error loading manifest: ${e}`;
      this.state.availableModules = [];
    } finally {
      this.state.loadingManifest = false;
      m.redraw();
    }
  }

  getResult(): ExtensionServer | null {
    if (!this.isUrlValid() || this.state.selectedModules.size === 0) {
      return null;
    }

    return {
      url: this.getCurrentUrl(),
      selectedModules: Array.from(this.state.selectedModules),
      enabled: this.editingServer?.enabled ?? true,
    };
  }

  view(): m.Children {
    return m(
      '.pf-add-extension-server-modal',
      m(
        Card,
        m(FormLabel, 'Server Type'),
        m(SegmentedButtons, {
          options: [
            {label: 'GitHub', icon: 'link'},
            {label: 'HTTP/HTTPS', icon: 'public'},
          ],
          selectedOption: this.state.serverType === 'github' ? 0 : 1,
          onOptionSelected: (idx: number) => {
            this.state.serverType = idx === 0 ? 'github' : 'http';
          },
        }),
      ),
      this.state.serverType === 'github'
        ? m(
            Card,
            m(FormLabel, 'Repository'),
            m(TextInput, {
              placeholder:
                'owner/repo (e.g., perfetto-dev/extension-server-test)',
              value: this.state.githubRepo,
              onInput: (value: string) => {
                this.state.githubRepo = value;
              },
            }),
            m(FormLabel, 'Branch/Tag'),
            m(TextInput, {
              placeholder: 'e.g., main',
              value: this.state.githubRef,
              onInput: (value: string) => {
                this.state.githubRef = value;
              },
            }),
          )
        : m(
            Card,
            m(FormLabel, 'URL'),
            m(TextInput, {
              placeholder: 'https://example.com/path/to/extensions',
              value: this.state.httpUrl,
              onInput: (value: string) => {
                this.state.httpUrl = value;
              },
            }),
          ),
      m(
        Card,
        m(
          '.pf-add-extension-server-modal__fetch-section',
          m(Button, {
            label: this.state.loadingManifest ? 'Loading...' : 'Fetch Manifest',
            disabled: !this.isUrlValid() || this.state.loadingManifest,
            onclick: () => this.loadManifest(),
          }),
          m(
            Tooltip,
            {
              trigger: m(Icon, {icon: 'help', className: 'pf-help-icon'}),
            },
            'Click "Fetch Manifest" to load available modules from the server. You must select at least one module to add the server.',
          ),
        ),
        this.state.manifestError &&
          m(
            '.pf-add-extension-server-modal__error',
            m(
              Callout,
              {icon: 'error', intent: Intent.Danger},
              this.state.manifestError,
            ),
          ),
        this.state.availableModules.length > 0 && [
          m(FormLabel, 'Select Modules'),
          m(MultiSelect, {
            options: this.state.availableModules.map((moduleName) => ({
              id: moduleName,
              name: moduleName,
              checked: this.state.selectedModules.has(moduleName),
            })),
            onChange: (diffs: MultiSelectDiff[]) => {
              for (const diff of diffs) {
                if (diff.checked) {
                  this.state.selectedModules.add(diff.id);
                } else {
                  this.state.selectedModules.delete(diff.id);
                }
              }
            },
            fixedSize: true,
          }),
        ],
      ),
    );
  }
}

export function showAddExtensionServerModal(
  server?: ExtensionServer,
): Promise<ExtensionServer | null> {
  return new Promise((resolve) => {
    const content = new AddExtensionServerModal(server);

    showModal({
      title: server ? 'Edit Extension Server' : 'Add Extension Server',
      buttons: [
        {
          text: server ? 'Save' : 'Add',
          primary: true,
          disabled: () => content.state.selectedModules.size === 0,
          action: () => {
            const result = content.getResult();
            if (result) {
              resolve(result);
            }
          },
        },
        {
          text: 'Cancel',
          primary: false,
          action: () => resolve(null),
        },
      ],
      content: () => content.view(),
    });
  });
}
