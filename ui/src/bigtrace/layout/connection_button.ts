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
import {Intent} from '../../widgets/common';
import {Popup, PopupPosition} from '../../widgets/popup';
import {TextInput} from '../../widgets/text_input';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {
  endpointStorage,
  getBigtraceEndpoint,
} from '../settings/endpoint_storage';

// Host of the configured endpoint, for the button label. Falls back to the raw
// string when it isn't a URL (a half-typed endpoint still reads sensibly).
function endpointLabel(endpoint: string): string {
  if (endpoint === '') return 'Connect backend';
  try {
    return new URL(endpoint).host;
  } catch {
    return endpoint;
  }
}

// The endpoint is the one piece of state that isn't per-query — it's a
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
      this.renderPanel(),
    );
  }

  private renderPanel(): m.Children {
    const setting = endpointStorage.get('bigtraceEndpoint');
    if (setting === undefined) return null;
    const loadError = bigTraceSettingsStorage.execConfigLoadError;
    return m('.pf-bt-connection-panel', [
      m('.pf-bt-connection-panel__title', 'BigTrace backend'),
      m(
        '.pf-bt-connection-panel__desc',
        'The service that holds the traces and runs your queries.',
      ),
      m(TextInput, {
        value: setting.get() as string,
        placeholder: 'https://your-bigtrace-backend/v1',
        className: 'pf-bt-connection-panel__input',
        oninput: (e: Event) => {
          setting.set((e.target as HTMLInputElement).value);
        },
      }),
      m('.pf-bt-connection-panel__actions', [
        // The endpoint is read once at module init, so a change needs a reload.
        endpointStorage.isReloadRequired() &&
          m(Button, {
            label: 'Reload to apply',
            icon: 'refresh',
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            onclick: () => window.location.reload(),
          }),
        m(Button, {
          label: 'Reset',
          icon: 'settings_backup_restore',
          title: 'Restore the default endpoint.',
          onclick: () => setting.reset(),
        }),
      ]),
      loadError !== undefined &&
        m(
          Callout,
          {intent: Intent.Danger, icon: 'error'},
          `Couldn't load settings from this backend: ${loadError}`,
        ),
    ]);
  }
}
