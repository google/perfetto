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
import {Button, ButtonVariant} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {CodeSnippet} from '../../widgets/code_snippet';
import {Intent} from '../../widgets/common';
import {Form, FormLabel} from '../../widgets/form';
import {Icon} from '../../widgets/icon';
import {Popup, PopupPosition} from '../../widgets/popup';
import {Spinner} from '../../widgets/spinner';
import {Stack, StackAuto} from '../../widgets/stack';
import {TextInput} from '../../widgets/text_input';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import {
  allowOriginCommand,
  installCommand,
  tunnelCommand,
} from '../settings/local_server_setup';
import {
  activeServer,
  addServer,
  type BigtraceServer,
  endpointProblem,
  isDefaultServerActive,
  isLoopbackEndpoint,
  LOCAL_ENDPOINT,
  listServers,
  removeServer,
  resetServer,
  sameEndpoint,
  selectServer,
  serverProblem,
  updateServer,
} from '../settings/server_list';
import {serverProbes} from '../settings/server_probe';

// How often loopback servers are asked whether they are up while the panel is
// open, so the local server shows up as soon as it starts.
const LOOPBACK_POLL_MS = 2000;

// Label of the topbar button: the server's name, else the endpoint's host.
function endpointLabel(endpoint: string): string {
  if (endpoint === '') return 'Connect backend';
  const server = activeServer();
  if (server !== undefined) return server.name;
  try {
    return new URL(endpoint).host;
  } catch {
    return endpoint;
  }
}

// Modules read the endpoint when they initialize, so a change needs a reload.
function reloadWith(endpoint: string): void {
  selectServer(endpoint);
  window.location.reload();
}

// The server is the one piece of state that isn't per-query — it's a
// connection, not a configuration — so it lives up here rather than in the
// per-tab settings. Auto-opens once when there's no working backend, which is
// the app's onboarding step now that the settings page is gone.
export class ConnectionButton implements m.ClassComponent {
  private open = false;
  private autoOpened = false;

  view(): m.Children {
    const endpoint = getBigtraceEndpoint();
    // The endpoint ships with a default, so "set" doesn't mean "reachable" —
    // a backend that failed to answer counts as not connected, otherwise this
    // button claims a connection the app doesn't have.
    const connected =
      endpoint !== '' &&
      bigTraceSettingsStorage.execConfigLoadError === undefined;
    if (
      !this.autoOpened &&
      !connected &&
      !bigTraceSettingsStorage.isExecConfigLoading
    ) {
      this.autoOpened = true;
      this.open = true;
    }
    return m(
      Popup,
      {
        trigger: m(Button, {
          icon: connected ? 'cloud_done' : 'cloud_off',
          label: endpointLabel(endpoint),
          title: connected
            ? endpoint
            : `Not connected to ${endpoint === '' ? 'a backend' : endpoint}`,
          onclick: () => {
            this.open = !this.open;
          },
        }),
        isOpen: this.open,
        onChange: (isOpen: boolean) => {
          this.open = isOpen;
        },
        position: PopupPosition.BottomEnd,
        className: 'pf-bt-connection-popup',
      },
      m(ServerPanel, {connected}),
    );
  }
}

interface ServerPanelAttrs {
  // Whether the active server answered the app.
  readonly connected: boolean;
}

// The add / edit form of a server. `editing` is the endpoint of the added
// server being edited; undefined when adding one.
interface ServerForm {
  editing?: string;
  name: string;
  endpoint: string;
  error?: string;
}

type ServerState = 'online' | 'offline' | 'checking';

const STATE_ICON: Record<ServerState, {intent: Intent; title: string}> = {
  online: {intent: Intent.Success, title: 'Answering'},
  offline: {intent: Intent.Danger, title: 'Not answering'},
  checking: {intent: Intent.None, title: 'Checking…'},
};

// The server picker: the servers with whether they answer, adding, editing
// and removing servers, going back to the default one, and the guide that
// sets up the local server and connects once it is up.
class ServerPanel implements m.ClassComponent<ServerPanelAttrs> {
  private timer?: ReturnType<typeof setInterval>;
  private setupOpen = false;
  private form?: ServerForm;
  // The endpoint field while edited; undefined shows the endpoint in use.
  private draft?: string;

  oncreate() {
    for (const s of listServers()) void serverProbes.refresh(s.endpoint);
    this.timer = setInterval(() => this.poll(), LOOPBACK_POLL_MS);
  }

  onremove() {
    clearInterval(this.timer);
  }

  private async poll() {
    for (const s of listServers()) {
      if (!isLoopbackEndpoint(s.endpoint)) continue;
      const result = await serverProbes.refresh(s.endpoint);
      // The guide's last step: switch to the local server once it is up.
      if (
        this.setupOpen &&
        s.kind === 'local' &&
        result?.state === 'online' &&
        !sameEndpoint(getBigtraceEndpoint(), s.endpoint)
      ) {
        reloadWith(s.endpoint);
      }
    }
  }

  view({attrs}: m.Vnode<ServerPanelAttrs>): m.Children {
    const loadError = bigTraceSettingsStorage.execConfigLoadError;
    const endpoint = getBigtraceEndpoint();
    const servers = listServers();
    const known = servers.some((s) => sameEndpoint(s.endpoint, endpoint));
    return m('.pf-bt-connection-panel', [
      m(Stack, {orientation: 'horizontal'}, [
        m('.pf-bt-connection-panel__title', 'BigTrace server'),
        m(StackAuto),
        m(Button, {
          label: 'Reset',
          icon: 'settings_backup_restore',
          compact: true,
          tooltip: 'Restore the default endpoint.',
          disabled: isDefaultServerActive() && this.draft === undefined,
          onclick: () => {
            if (isDefaultServerActive()) {
              this.draft = undefined;
              return;
            }
            resetServer();
            window.location.reload();
          },
        }),
      ]),
      m(
        '.pf-bt-connection-panel__desc',
        'The server that holds the traces and runs your queries.',
      ),
      this.renderEndpointField(),
      m('.pf-bt-server-list', [
        servers.map((s) => this.renderServer(s, attrs.connected)),
        // An endpoint from ?server= or an older version that isn't listed.
        !known &&
          endpoint !== '' &&
          this.renderServer(
            {name: 'Current server', endpoint, kind: 'custom'},
            attrs.connected,
            /* unsaved= */ true,
          ),
      ]),
      this.form !== undefined
        ? this.renderForm(this.form)
        : this.renderActions(),
      this.setupOpen && this.renderSetup(),
      loadError !== undefined &&
        m(
          Callout,
          {intent: Intent.Danger, icon: 'error'},
          `Couldn't load settings from ${endpoint}: ${loadError}`,
        ),
      // A local server installed without this UI's origin refuses it.
      loadError?.includes('Origin not allowed') &&
        isLoopbackEndpoint(endpoint) && [
          m(
            '.pf-bt-local-setup__step',
            'This server does not accept requests from this page. On its ' +
              'machine, allow it with:',
          ),
          m(CodeSnippet, {text: allowOriginCommand(window.location.origin)}),
        ],
    ]);
  }

  private renderServer(
    s: BigtraceServer,
    connected: boolean,
    unsaved = false,
  ): m.Children {
    const active = sameEndpoint(s.endpoint, getBigtraceEndpoint());
    const probe = serverProbes.get(s.endpoint);
    const state: ServerState =
      active && connected ? 'online' : (probe?.state ?? 'checking');
    const info = probe?.state === 'online' ? probe.info : undefined;
    const detail =
      info?.traceCount !== undefined
        ? `${info.traceCount} traces in ${info.traceDir ?? '?'}`
        : s.endpoint;
    // Actions don't select the row they sit in.
    const action = (fn: () => void) => (e: MouseEvent) => {
      e.stopPropagation();
      fn();
    };
    return m(
      '.pf-bt-server',
      {
        'className': active ? 'pf-bt-server--active' : '',
        'data-server-kind': s.kind,
        'role': 'button',
        'tabindex': 0,
        'title': active ? `Connected to ${s.endpoint}` : `Use ${s.endpoint}`,
        'onclick': () => {
          if (!active) reloadWith(s.endpoint);
        },
        'onkeydown': (e: KeyboardEvent) => {
          if ((e.key === 'Enter' || e.key === ' ') && !active) {
            reloadWith(s.endpoint);
          }
        },
      },
      [
        m(Icon, {
          icon: 'circle',
          filled: true,
          className: 'pf-bt-server__state',
          intent: STATE_ICON[state].intent,
          title: STATE_ICON[state].title,
        }),
        m('.pf-bt-server__text', [
          m('.pf-bt-server__name', s.name),
          m('.pf-bt-server__detail', detail),
        ]),
        // The selected server, as menus mark theirs.
        active && m(Icon, {icon: 'check', title: 'In use'}),
        s.kind === 'local' &&
          state === 'offline' &&
          !this.setupOpen &&
          m(Button, {
            label: 'Set up',
            icon: 'build',
            compact: true,
            intent: Intent.Primary,
            onclick: action(() => (this.setupOpen = true)),
          }),
        unsaved &&
          m(Button, {
            icon: 'bookmark_add',
            tooltip: 'Add to the list',
            compact: true,
            onclick: action(
              () => (this.form = {name: '', endpoint: s.endpoint}),
            ),
          }),
        s.kind === 'custom' &&
          !unsaved && [
            m(Button, {
              icon: 'edit',
              tooltip: 'Edit',
              compact: true,
              onclick: action(
                () =>
                  (this.form = {
                    editing: s.endpoint,
                    name: s.name,
                    endpoint: s.endpoint,
                  }),
              ),
            }),
            m(Button, {
              icon: 'delete',
              tooltip: 'Remove',
              compact: true,
              onclick: action(() => removeServer(s.endpoint)),
            }),
          ],
      ],
    );
  }

  private renderActions(): m.Children {
    return m('.pf-bt-connection-panel__actions', [
      m(Button, {
        label: 'Add server',
        icon: 'add',
        onclick: () => (this.form = {name: '', endpoint: ''}),
      }),
    ]);
  }

  // The endpoint in use, editable as before the picker: type any URL and
  // apply it, or reset to the default server. An endpoint that isn't listed
  // shows up as "Current server", which can be added to the list.
  private renderEndpointField(): m.Children {
    const current = getBigtraceEndpoint();
    const draft = this.draft ?? current;
    const changed = !sameEndpoint(draft, current) && draft.trim() !== '';
    const problem = changed ? endpointProblem(draft) : undefined;
    const apply = () => {
      if (changed && problem === undefined) reloadWith(draft);
    };
    return [
      m(TextInput, {
        value: draft,
        placeholder: 'https://your-bigtrace-backend/v1',
        className: 'pf-bt-connection-panel__input',
        oninput: (e: Event) => {
          this.draft = (e.target as HTMLInputElement).value;
        },
        onkeydown: (e: KeyboardEvent) => {
          if (e.key === 'Enter') apply();
        },
      }),
      problem !== undefined &&
        m(Callout, {intent: Intent.Danger, icon: 'error'}, problem),
      // The endpoint is read once at module init, so a change needs a reload.
      changed &&
        m(
          '.pf-bt-connection-panel__actions',
          m(Button, {
            label: 'Reload to apply',
            icon: 'refresh',
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            disabled: problem !== undefined,
            onclick: apply,
          }),
        ),
    ];
  }

  private renderForm(form: ServerForm): m.Children {
    const problem =
      form.endpoint.trim() === ''
        ? undefined
        : serverProblem(form.endpoint, form.editing);
    const submit = () => {
      try {
        if (form.editing === undefined) {
          const server = addServer(form.name, form.endpoint);
          void serverProbes.refresh(server.endpoint);
        } else if (updateServer(form.editing, form.name, form.endpoint)) {
          window.location.reload();
          return;
        }
        this.form = undefined;
      } catch (e) {
        form.error = e instanceof Error ? e.message : String(e);
      }
    };
    return m(
      Form,
      {
        className: 'pf-bt-server-form',
        submitLabel: form.editing === undefined ? 'Add' : 'Save',
        submitIcon: form.editing === undefined ? 'add' : 'check',
        cancelLabel: 'Cancel',
        onSubmit: submit,
        validation: () => form.endpoint.trim() !== '' && problem === undefined,
      },
      [
        m(FormLabel, 'Name'),
        m(TextInput, {
          value: form.name,
          placeholder: 'Optional, e.g. Alice’s workstation',
          oninput: (e: Event) => {
            form.name = (e.target as HTMLInputElement).value;
          },
        }),
        m(FormLabel, 'URL'),
        m(TextInput, {
          value: form.endpoint,
          placeholder: 'http://127.0.0.1:5002/v1',
          autofocus: true,
          oninput: (e: Event) => {
            form.endpoint = (e.target as HTMLInputElement).value;
            form.error = undefined;
          },
        }),
        (problem ?? form.error) !== undefined &&
          m(
            Callout,
            {intent: Intent.Danger, icon: 'error'},
            problem ?? form.error,
          ),
      ],
    );
  }

  private renderSetup(): m.Children {
    const local = serverProbes.get(LOCAL_ENDPOINT);
    return m('.pf-bt-local-setup', [
      m('.pf-bt-local-setup__title', 'Set up the local server'),
      m('.pf-bt-local-setup__step', [
        '1. On the Linux machine with your traces (e.g. your workstation), ' +
          'install the always-on server. It serves ~/traces by default ' +
          '(add ',
        m('code', '-- --traces DIR'),
        ' to change it) and keeps running across restarts; ',
        m('code', 'bigtrace status | stop | start | logs'),
        ' control it.',
      ]),
      m(CodeSnippet, {text: installCommand(window.location.origin)}),
      m(
        '.pf-bt-local-setup__step',
        '2. If this browser runs on another machine (e.g. a laptop), forward ' +
          'the port from it, and keep that running:',
      ),
      m(CodeSnippet, {text: tunnelCommand()}),
      m('.pf-bt-local-setup__hint', [
        'or, with the bigtrace command installed there too: ',
        m('code', 'bigtrace tunnel YOUR_WORKSTATION'),
        '. Your browser may ask to let this site access your local network: ' +
          'allow it.',
      ]),
      m('.pf-bt-local-setup__status', [
        local?.state === 'online'
          ? 'Local server found. Connecting…'
          : [m(Spinner), ` Waiting for ${LOCAL_ENDPOINT}…`],
      ]),
      m('.pf-bt-connection-panel__actions', [
        m(Button, {
          label: 'Close',
          onclick: () => {
            this.setupOpen = false;
          },
        }),
      ]),
    ]);
  }
}
