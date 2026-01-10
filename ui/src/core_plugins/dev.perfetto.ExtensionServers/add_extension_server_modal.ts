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

import m from 'mithril';
import {AsyncLimiter} from '../../base/async_limiter';
import {Button} from '../../widgets/button';
import {showModal} from '../../widgets/modal';
import {TextInput} from '../../widgets/text_input';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {FormLabel} from '../../widgets/form';
import {MultiSelect, MultiSelectDiff} from '../../widgets/multiselect';
import {ExtensionServer} from './types';
import {defer} from '../../base/deferred';
import {resolveServerUrl} from './url_utils';
import {loadManifest} from './extension_server';
import {Stack} from '../../widgets/stack';
import {EmptyState} from '../../widgets/empty_state';
import {Section} from '../../widgets/section';
import {debounce} from '../../frontend/rate_limiters';

interface GithubUserInput {
  type: 'github';
  repo: string;
  ref: string;
}

interface HttpsUserInput {
  type: 'https';
  url: string;
}

type UserInput = GithubUserInput | HttpsUserInput;

interface OkLoadedState {
  type: 'ok';
  availableModules: ReadonlyArray<string>;
  enabledModules: Set<string>;
}

interface ErrorLoadedState {
  type: 'error';
  error: string;
}

type LoadedState = OkLoadedState | ErrorLoadedState;

class AddExtensionServerModal {
  private readonly fetchLimiter = new AsyncLimiter();
  private readonly debouncedFetch = debounce(
    () => this.scheduleManifestFetch(),
    250,
  );
  private userInput: UserInput;
  private loadedState?: LoadedState;

  constructor(server?: ExtensionServer) {
    this.userInput = createInitial(server);
    this.scheduleManifestFetch(server?.enabledModules);
  }

  view() {
    return m(
      '.pf-add-extension-server-modal',
      m(
        Section,
        {title: 'Server'},
        m(
          Stack,
          {gap: 'small'},
          m(SegmentedButtons, {
            options: [
              {label: 'GitHub', icon: 'link'},
              {label: 'HTTPS', icon: 'public'},
            ],
            selectedOption: this.userInput.type === 'github' ? 0 : 1,
            onOptionSelected: (idx: number) => {
              if (idx === 0 && this.userInput.type !== 'github') {
                this.userInput = {type: 'github', repo: '', ref: 'main'};
                this.scheduleManifestFetch();
              } else if (idx === 1 && this.userInput.type !== 'https') {
                this.userInput = {type: 'https', url: ''};
                this.scheduleManifestFetch();
              }
            },
          }),
          this.userInput.type === 'github'
            ? [
                m(FormLabel, 'Repository'),
                m(TextInput, {
                  placeholder:
                    'owner/repo (e.g., perfetto-dev/extension-server-test)',
                  value: this.userInput.repo,
                  onInput: (value: string) => {
                    const github = assertGithub(this.userInput);
                    github.repo = value;
                    this.debouncedFetch();
                  },
                }),
                m(FormLabel, 'Branch/Tag'),
                m(TextInput, {
                  placeholder: 'e.g., main',
                  value: this.userInput.ref,
                  onInput: (value: string) => {
                    const github = assertGithub(this.userInput);
                    github.ref = value;
                    this.debouncedFetch();
                  },
                }),
              ]
            : [
                m(FormLabel, 'URL'),
                m(TextInput, {
                  placeholder: 'https://example.com/path/to/extensions',
                  value: this.userInput.url,
                  onInput: (value: string) => {
                    const https = assertHttps(this.userInput);
                    https.url = value;
                    this.debouncedFetch();
                  },
                }),
              ],
        ),
      ),
      m(
        Section,
        {
          title: [
            m('span', 'Modules'),
            (this.loadedState?.type === 'ok' ||
              this.loadedState?.type === 'error') &&
              m(Button, {
                icon: 'refresh',
                compact: true,
                onclick: () => this.scheduleManifestFetch(),
              }),
          ],
        },
        this.renderModuleSection(),
      ),
    );
  }

  canSave(): boolean {
    return (
      this.loadedState?.type === 'ok' &&
      this.loadedState.enabledModules.size > 0
    );
  }

  getResult(): ExtensionServer | undefined {
    if (this.loadedState?.type !== 'ok') {
      return undefined;
    }
    return {
      url: this.getUrl(),
      enabledModules: Array.from(
        assertOkLoadedState(this.loadedState).enabledModules,
      ),
      enabled: true,
    };
  }

  private scheduleManifestFetch(
    preserveEnabledModules?: ReadonlyArray<string>,
  ): void {
    this.loadedState = undefined;
    this.fetchLimiter.schedule(() => this.loadManifest(preserveEnabledModules));
  }

  private async loadManifest(
    preserveEnabledModules?: ReadonlyArray<string>,
  ): Promise<void> {
    if (this.isEmptyInput()) {
      return;
    }

    // Validate HTTPS URL protocol
    if (this.userInput.type === 'https') {
      const validationError = this.validateHttpsUrl();
      if (validationError) {
        this.loadedState = {type: 'error', error: validationError};
        m.redraw();
        return;
      }
    }

    const url = this.getUrl();
    const resolvedUrl = resolveServerUrl(url);
    const manifestResult = await loadManifest(resolvedUrl);
    if (!manifestResult.ok) {
      const location =
        this.userInput.type === 'github'
          ? `${this.userInput.repo} @ ${this.userInput.ref}`
          : this.userInput.url;
      const hint =
        this.userInput.type === 'github'
          ? 'Please check that the repository, branch, and manifest.json file exist.'
          : 'Check that the URL is correct and CORS is enabled.';
      this.loadedState = {
        type: 'error',
        error: `Could not fetch manifest from ${location}. ${hint}`,
      };
      console.warn(
        `Error fetching manifest from ${resolvedUrl}: ${manifestResult.error}`,
      );
      m.redraw();
      return;
    }

    // Determine which modules to enable
    const manifest = manifestResult.value;
    let enabledModules: Set<string>;
    if (preserveEnabledModules) {
      // When editing, preserve enabled modules that still exist in manifest
      enabledModules = new Set(
        preserveEnabledModules.filter((m) => manifest.modules.includes(m)),
      );
    } else {
      // When creating new, default to 'default' module if available
      enabledModules = new Set(
        manifest.modules.includes('default') ? ['default'] : [],
      );
    }

    this.loadedState = {
      type: 'ok',
      availableModules: manifest.modules,
      enabledModules,
    };
    m.redraw();
  }

  private getUrl() {
    if (this.userInput.type === 'github') {
      return `github://${this.userInput.repo}/${this.userInput.ref}`;
    }
    const url = this.userInput.url.trim();
    // Auto-add https:// if no protocol specified
    if (!url.includes('://')) {
      return `https://${url}`;
    }
    return url;
  }

  private validateHttpsUrl(): string | undefined {
    if (this.userInput.type !== 'https') return undefined;
    const url = this.userInput.url.trim();
    if (url === '') return undefined;
    // Check for invalid protocol prefixes
    if (url.includes('://') && !url.startsWith('https://')) {
      return 'URL must use https:// protocol';
    }
    return undefined;
  }

  private isEmptyInput(): boolean {
    if (this.userInput.type === 'github') {
      // Both repo and ref are required
      return (
        this.userInput.repo.trim() === '' || this.userInput.ref.trim() === ''
      );
    } else {
      return this.userInput.url.trim() === '';
    }
  }

  private renderModuleSection(): m.Children {
    // Empty input state
    if (this.isEmptyInput()) {
      return m(EmptyState, {
        icon: 'extension',
        title: 'Enter server details above to load available modules',
      });
    }

    // Loading state
    if (this.loadedState === undefined) {
      return m(EmptyState, {
        icon: 'hourglass_empty',
        title: 'Fetching manifest...',
      });
    }

    // Error state
    if (this.loadedState.type === 'error') {
      return m(EmptyState, {
        icon: 'error',
        title: this.loadedState.error,
      });
    }

    // Success but no modules
    if (this.loadedState.availableModules.length === 0) {
      return m(EmptyState, {
        icon: 'inbox',
        title: 'No modules available in this extension server',
      });
    }

    // Success with modules
    return m(MultiSelect, {
      options: this.loadedState.availableModules.map((moduleName) => ({
        id: moduleName,
        name: moduleName,
        checked: assertOkLoadedState(this.loadedState).enabledModules.has(
          moduleName,
        ),
      })),
      onChange: (diffs: MultiSelectDiff[]) => {
        const ok = assertOkLoadedState(this.loadedState);
        for (const diff of diffs) {
          if (diff.checked) {
            ok.enabledModules.add(diff.id);
          } else {
            ok.enabledModules.delete(diff.id);
          }
        }
      },
    });
  }
}

export function showAddExtensionServerModal(
  server?: ExtensionServer,
): Promise<ExtensionServer | undefined> {
  const deferred = defer<ExtensionServer | undefined>();
  const modal = new AddExtensionServerModal(server);
  showModal({
    title: server ? 'Edit Extension Server' : 'Add Extension Server',
    buttons: [
      {
        text: server ? 'Save' : 'Add',
        primary: true,
        disabled: () => !modal.canSave(),
        action: () => deferred.resolve(modal.getResult()),
      },
      {
        text: 'Cancel',
        primary: false,
        action: () => deferred.resolve(undefined),
      },
    ],
    content: () => modal.view(),
  });
  return deferred;
}

function createInitial(server?: ExtensionServer): UserInput {
  if (!server) {
    return {
      type: 'github',
      repo: '',
      ref: 'main',
    };
  }
  const githubMatch = server.url.match(/^github:\/\/([^/]+\/[^/]+)\/(.+)$/);
  if (githubMatch) {
    return {
      type: 'github',
      repo: githubMatch[1] ?? '',
      ref: githubMatch[2] ?? 'main',
    };
  }
  const httpsMatch = server.url.match(/^https?:\/\/(.+)$/);
  if (httpsMatch) {
    return {
      type: 'https',
      url: httpsMatch[1],
    };
  }
  throw new Error(`Unsupported server URL: ${server.url}`);
}

function assertGithub(state: UserInput): GithubUserInput {
  if (state.type !== 'github') {
    throw new Error('State is not of type GithubState');
  }
  return state;
}

function assertHttps(state: UserInput): HttpsUserInput {
  if (state.type !== 'https') {
    throw new Error('State is not of type HttpsState');
  }
  return state;
}

function assertOkLoadedState(state: LoadedState | undefined): OkLoadedState {
  if (state?.type !== 'ok') {
    throw new Error('LoadedState is not of type OkLoadedState');
  }
  return state;
}
