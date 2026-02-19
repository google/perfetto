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
import {Select} from '../../widgets/select';
import {Form, FormLabel, FormSection} from '../../widgets/form';
import {MultiSelect, MultiSelectDiff} from '../../widgets/multiselect';
import {ExtensionServer, UserInput} from './types';
import {defer} from '../../base/deferred';
import {loadManifest} from './extension_server';
import {normalizeHttpsUrl} from './url_utils';
import {Icon} from '../../widgets/icon';
import {Anchor} from '../../widgets/anchor';
import {Popup} from '../../widgets/popup';
import {EmptyState} from '../../widgets/empty_state';
import {debounce} from '../../base/rate_limiters';

type GithubUserInput = Extract<UserInput, {type: 'github'}>;
type HttpsUserInput = Extract<UserInput, {type: 'https'}>;

interface OkLoadedState {
  type: 'ok';
  availableModules: ReadonlyArray<{id: string; name: string}>;
  enabledModules: Set<string>;
}

interface ErrorLoadedState {
  type: 'error';
  error: string;
}

type LoadedState = OkLoadedState | ErrorLoadedState;

const PAT_HELP_URL =
  'https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/managing-your-personal-access-tokens';

class AddExtensionServerModal {
  private readonly fetchLimiter = new AsyncLimiter();
  private readonly debouncedFetch = debounce(
    () => this.scheduleManifestFetch(),
    250,
  );
  private userInput: UserInput;
  private loadedState?: LoadedState;
  private readonly locked: boolean;

  constructor(server?: ExtensionServer, prefill?: ExtensionServer) {
    this.userInput = createInitial(server ?? prefill);
    this.locked = server?.locked ?? false;
    this.scheduleManifestFetch(
      server?.enabledModules ?? prefill?.enabledModules,
    );
  }

  view() {
    return m(
      '.pf-add-extension-server-modal',
      m(Form, {}, this.renderServerSection(), this.renderModuleSection()),
    );
  }

  canSave(): boolean {
    return (
      !this.isEmptyInput() &&
      this.loadedState?.type === 'ok' &&
      this.loadedState.enabledModules.size > 0
    );
  }

  getResult(): ExtensionServer | undefined {
    if (this.loadedState?.type !== 'ok') {
      return undefined;
    }
    const enabledModules = Array.from(this.loadedState.enabledModules);
    if (this.userInput.type === 'github') {
      return {
        type: 'github',
        repo: this.userInput.repo,
        ref: this.userInput.ref,
        path: this.userInput.path.trim() || '/',
        enabledModules,
        enabled: true,
        auth: this.userInput.auth,
        locked: this.locked,
      };
    }
    return {
      type: 'https',
      url: normalizeHttpsUrl(this.userInput.url),
      enabledModules,
      enabled: true,
      auth: this.userInput.auth,
      locked: this.locked,
    };
  }

  private renderServerSection(): m.Children {
    return m(
      FormSection,
      {label: 'Server'},
      this.renderServerTypePicker(),
      this.userInput.type === 'github'
        ? this.renderGithubFields(this.userInput)
        : this.renderHttpsFields(this.userInput),
    );
  }

  private renderServerTypePicker(): m.Children {
    return m(SegmentedButtons, {
      options: [
        {label: 'GitHub', icon: 'link'},
        {label: 'HTTPS', icon: 'public'},
      ],
      selectedOption: this.userInput.type === 'github' ? 0 : 1,
      disabled: this.locked,
      onOptionSelected: (idx: number) => {
        if (idx === 0 && this.userInput.type !== 'github') {
          this.userInput = {
            type: 'github',
            repo: '',
            ref: 'main',
            path: '',
            auth: {type: 'none'},
          };
          this.scheduleManifestFetch();
        } else if (idx === 1 && this.userInput.type !== 'https') {
          this.userInput = {type: 'https', url: '', auth: {type: 'none'}};
          this.scheduleManifestFetch();
        }
      },
    });
  }

  private renderGithubFields(input: GithubUserInput): m.Children {
    return [
      m(FormLabel, 'Repository'),
      m(TextInput, {
        placeholder: 'owner/repo (e.g., perfetto-dev/extension-server-test)',
        value: input.repo,
        disabled: this.locked,
        onInput: (value: string) => {
          input.repo = value;
          this.debouncedFetch();
        },
      }),
      m(FormLabel, 'Branch/Tag'),
      m(TextInput, {
        placeholder: 'e.g., main',
        value: input.ref,
        disabled: this.locked,
        onInput: (value: string) => {
          input.ref = value;
          this.debouncedFetch();
        },
      }),
      m(FormLabel, 'Path'),
      m(TextInput, {
        placeholder: '(default: /)',
        value: input.path,
        disabled: this.locked,
        onInput: (value: string) => {
          input.path = value;
          this.debouncedFetch();
        },
      }),
      this.renderGithubAuth(input),
    ];
  }

  private renderGithubAuth(input: GithubUserInput): m.Children {
    return [
      m(FormLabel, [
        'Authentication ',
        m(
          Popup,
          {
            trigger: m(Icon, {icon: 'help_outline'}),
            closeOnOutsideClick: true,
          },
          m(
            'span',
            'A GitHub ',
            m(
              Anchor,
              {href: PAT_HELP_URL, target: '_blank'},
              'Personal Access Token',
            ),
            ' is required to access private repositories.',
          ),
        ),
      ]),
      m(
        Select,
        {
          value: input.auth.type,
          disabled: this.locked,
          onchange: (e: Event) => {
            const value = (e.target as HTMLSelectElement).value;
            input.auth =
              value === 'github_pat'
                ? {type: 'github_pat', pat: ''}
                : {type: 'none'};
            this.debouncedFetch();
          },
        },
        m('option', {value: 'none'}, 'None'),
        m('option', {value: 'github_pat'}, 'Personal Access Token'),
      ),
      input.auth.type === 'github_pat' &&
        m(TextInput, {
          placeholder: 'github_pat_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
          type: 'password',
          value: input.auth.pat,
          disabled: this.locked,
          onInput: (value: string) => {
            input.auth = {type: 'github_pat', pat: value};
            this.debouncedFetch();
          },
        }),
    ];
  }

  private renderHttpsFields(input: HttpsUserInput): m.Children {
    return [
      m(FormLabel, 'URL'),
      m(TextInput, {
        placeholder: 'https://example.com/path/to/extensions',
        value: input.url,
        disabled: this.locked,
        onInput: (value: string) => {
          input.url = value;
          this.debouncedFetch();
        },
      }),
      this.renderHttpsAuth(input),
    ];
  }

  private renderHttpsAuth(input: HttpsUserInput): m.Children {
    return [
      m(FormLabel, [
        'Authentication ',
        m(
          Popup,
          {
            trigger: m(Icon, {icon: 'help_outline'}),
            closeOnOutsideClick: true,
          },
          m(
            'span',
            m('strong', 'None'),
            ': no authentication headers are sent.',
            m('br'),
            m('strong', 'Basic'),
            ': sends a Base64-encoded username:password via the ',
            m('code', 'Authorization'),
            ' header.',
            m('br'),
            m('strong', 'API Key'),
            ': sends a token via a configurable HTTP header.',
            m('br'),
            m('strong', 'SSO (Cookie)'),
            ': uses browser cookies for authentication. On 403, refreshes cookies via an iframe redirect.',
          ),
        ),
      ]),
      m(
        Select,
        {
          value: input.auth.type,
          disabled: this.locked,
          onchange: (e: Event) => {
            const value = (e.target as HTMLSelectElement).value;
            if (value === 'https_basic') {
              input.auth = {type: 'https_basic', username: '', password: ''};
            } else if (value === 'https_apikey') {
              input.auth = {
                type: 'https_apikey',
                keyType: 'bearer',
                key: '',
                customHeaderName: '',
              };
            } else if (value === 'https_sso') {
              input.auth = {type: 'https_sso'};
            } else {
              input.auth = {type: 'none'};
            }
            this.debouncedFetch();
          },
        },
        m('option', {value: 'none'}, 'None'),
        m('option', {value: 'https_basic'}, 'Basic'),
        m('option', {value: 'https_apikey'}, 'API Key'),
        m('option', {value: 'https_sso'}, 'SSO (Cookie)'),
      ),
      input.auth.type === 'https_basic' && this.renderBasicAuthFields(input),
      input.auth.type === 'https_apikey' && this.renderApiKeyFields(input),
    ];
  }

  private renderBasicAuthFields(input: HttpsUserInput): m.Children {
    if (input.auth.type !== 'https_basic') return null;
    const auth = input.auth;
    return [
      m(TextInput, {
        placeholder: 'Username',
        value: auth.username,
        disabled: this.locked,
        onInput: (value: string) => {
          input.auth = {...auth, username: value};
          this.debouncedFetch();
        },
      }),
      m(TextInput, {
        placeholder: 'Password',
        type: 'password',
        value: auth.password,
        disabled: this.locked,
        onInput: (value: string) => {
          input.auth = {...auth, password: value};
          this.debouncedFetch();
        },
      }),
    ];
  }

  private renderApiKeyFields(input: HttpsUserInput): m.Children {
    if (input.auth.type !== 'https_apikey') return null;
    const auth = input.auth;
    return [
      m(FormLabel, [
        'Header Format ',
        m(
          Popup,
          {
            trigger: m(Icon, {icon: 'help_outline'}),
            closeOnOutsideClick: true,
          },
          m(
            'span',
            m('strong', 'Bearer'),
            ': sends ',
            m('code', 'Authorization: Bearer <key>'),
            '.',
            m('br'),
            m('strong', 'X-API-Key'),
            ': sends ',
            m('code', 'X-API-Key: <key>'),
            '.',
            m('br'),
            m('strong', 'Custom'),
            ': sends the key via a custom header name you specify.',
          ),
        ),
      ]),
      m(
        Select,
        {
          value: auth.keyType,
          disabled: this.locked,
          onchange: (e: Event) => {
            const keyType = (e.target as HTMLSelectElement).value as
              | 'bearer'
              | 'x_api_key'
              | 'custom';
            input.auth = {
              ...auth,
              keyType,
              customHeaderName: '',
            } as typeof auth;
            this.debouncedFetch();
          },
        },
        m('option', {value: 'bearer'}, 'Bearer'),
        m('option', {value: 'x_api_key'}, 'X-API-Key'),
        m('option', {value: 'custom'}, 'Custom'),
      ),
      auth.keyType === 'custom' &&
        m(TextInput, {
          placeholder: 'Header name',
          value: auth.customHeaderName,
          disabled: this.locked,
          onInput: (value: string) => {
            input.auth = {...auth, customHeaderName: value};
            this.debouncedFetch();
          },
        }),
      m(TextInput, {
        placeholder: 'API key',
        type: 'password',
        value: auth.key,
        disabled: this.locked,
        onInput: (value: string) => {
          input.auth = {...auth, key: value};
          this.debouncedFetch();
        },
      }),
    ];
  }

  private renderModuleSection(): m.Children {
    const showRefresh =
      !this.isEmptyInput() &&
      (this.loadedState?.type === 'ok' || this.loadedState?.type === 'error');
    return m(
      FormSection,
      {label: 'Modules'},
      showRefresh &&
        m(
          '.pf-add-extension-server-modal__refresh',
          m(Button, {
            icon: 'refresh',
            compact: true,
            onclick: () => this.scheduleManifestFetch(),
          }),
        ),
      this.renderModuleContent(),
    );
  }

  private renderModuleContent(): m.Children {
    if (this.isEmptyInput()) {
      return m(EmptyState, {
        icon: 'extension',
        title: 'Enter server details above to load available modules',
      });
    }
    if (this.loadedState === undefined) {
      return m(EmptyState, {
        icon: 'hourglass_empty',
        title: 'Fetching manifest...',
      });
    }
    if (this.loadedState.type === 'error') {
      return m(EmptyState, {
        icon: 'error',
        title: this.loadedState.error,
      });
    }
    if (this.loadedState.availableModules.length === 0) {
      return m(EmptyState, {
        icon: 'inbox',
        title: 'No modules available in this extension server',
      });
    }
    const {enabledModules, availableModules} = this.loadedState;
    return m(MultiSelect, {
      options: availableModules.map(({id, name}) => ({
        id,
        name,
        checked: enabledModules.has(id),
      })),
      onChange: (diffs: MultiSelectDiff[]) => {
        for (const diff of diffs) {
          if (diff.checked) {
            enabledModules.add(diff.id);
          } else {
            enabledModules.delete(diff.id);
          }
        }
      },
    });
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

    const manifestResult = await loadManifest(this.userInput);
    if (!manifestResult.ok) {
      const location =
        this.userInput.type === 'github'
          ? `${this.userInput.repo} @ ${this.userInput.ref}`
          : this.userInput.url;
      const hint =
        this.userInput.type === 'github'
          ? 'Please check that the repository, branch, and manifest endpoint exist.'
          : 'Check that the URL is correct and CORS is enabled.';
      this.loadedState = {
        type: 'error',
        error: `Could not fetch manifest from ${location}. ${hint}`,
      };
      console.warn(`Error fetching manifest: ${manifestResult.error}`);
      m.redraw();
      return;
    }

    const manifest = manifestResult.value;
    const enabledModules = preserveEnabledModules
      ? new Set(
          preserveEnabledModules.filter((m) =>
            manifest.modules.some((mod) => mod.id === m),
          ),
        )
      : new Set(
          manifest.modules.some((mod) => mod.id === 'default')
            ? ['default']
            : [],
        );

    this.loadedState = {
      type: 'ok',
      availableModules: manifest.modules,
      enabledModules,
    };
    m.redraw();
  }

  private validateHttpsUrl(): string | undefined {
    if (this.userInput.type !== 'https') return undefined;
    const url = this.userInput.url.trim();
    if (url === '') return undefined;
    if (url.includes('://') && !url.startsWith('https://')) {
      return 'URL must use https:// protocol';
    }
    return undefined;
  }

  private isEmptyInput(): boolean {
    if (this.userInput.type === 'github') {
      if (
        this.userInput.repo.trim() === '' ||
        this.userInput.ref.trim() === ''
      ) {
        return true;
      }
      // PAT auth requires a non-empty token.
      const auth = this.userInput.auth;
      if (auth.type === 'github_pat' && !auth.pat.trim()) {
        return true;
      }
      return false;
    }
    if (this.userInput.url.trim() === '') return true;
    const auth = this.userInput.auth;
    // Basic auth requires both username and password.
    if (
      auth.type === 'https_basic' &&
      (!auth.username.trim() || !auth.password.trim())
    ) {
      return true;
    }
    // API key auth requires a non-empty key (and header name for custom).
    if (auth.type === 'https_apikey') {
      if (!auth.key.trim()) return true;
      if (auth.keyType === 'custom' && !auth.customHeaderName.trim()) {
        return true;
      }
    }
    return false;
  }
}

export interface ShowModalOpts {
  existingServer?: ExtensionServer; // Edit mode
  prefill?: ExtensionServer; // Add mode with defaults (e.g. from shared link)
}

export function showAddExtensionServerModal(
  opts?: ShowModalOpts,
): Promise<ExtensionServer | undefined> {
  const deferred = defer<ExtensionServer | undefined>();
  const existing = opts?.existingServer;
  const modal = new AddExtensionServerModal(existing, opts?.prefill);
  showModal({
    title: existing ? 'Edit Extension Server' : 'Add Extension Server',
    buttons: [
      {
        text: existing ? 'Save' : 'Add',
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
      path: '',
      auth: {type: 'none'},
    };
  }
  if (server.type === 'github') {
    return {
      type: 'github',
      repo: server.repo,
      ref: server.ref,
      path: server.path === '/' ? '' : server.path,
      auth: server.auth,
    };
  }
  return {
    type: 'https',
    url: server.url.replace(/^https:\/\//, ''),
    auth: server.auth,
  };
}
