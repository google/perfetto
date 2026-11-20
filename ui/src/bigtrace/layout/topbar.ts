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

import '../../frontend/topbar.scss';
import m from 'mithril';
import {assetSrc} from '../../base/assets';
import {Button} from '../../widgets/button';
import {Chip} from '../../widgets/chip';
import {Intent} from '../../widgets/common';
import {Icon} from '../../widgets/icon';
import {settingsStorage} from '../settings/settings_storage';
import {MenuItem, MenuTitle, PopupMenu} from '../../widgets/menu';
import {PopupPosition} from '../../widgets/popup';
import {toggleHelp} from '../help_modal';
import {
  getDeployedReleaseChannels,
  getReleaseChannel,
  switchReleaseChannel,
  type ReleaseChannel,
} from '../settings/release_channel';
import {ConnectionButton} from './connection_button';

const CHANNELS: Record<
  ReleaseChannel,
  {label: string; description: string; intent: Intent}
> = {
  stable: {label: 'Stable', description: 'Default', intent: Intent.None},
  canary: {
    label: 'Canary',
    description: 'Newer, less tested',
    intent: Intent.Warning,
  },
  autopush: {
    label: 'Autopush',
    description: 'Newest, can break',
    intent: Intent.Danger,
  },
};

// The app's only chrome: brand, command omnibox, and the backend connection.
export class Topbar implements m.ClassComponent {
  view() {
    return m('.pf-topbar', [
      m('.pf-bt-topbar-brand', [
        m('img.pf-bt-topbar-logo', {src: assetSrc('assets/logo-128.png')}),
        'BigTrace',
        renderChannelPicker(),
      ]),
      m('.pf-topbar__right', [
        m(ConnectionButton),
        // Two rare actions, so one door rather than two: the topbar stays
        // the connection and nothing else, and help has somewhere to be
        // found again.
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              icon: 'more_vert',
              title: 'More',
            }),
            position: PopupPosition.BottomEnd,
          },
          m(MenuItem, {
            // Names the theme you'd get, not the one you're in.
            label: isDark() ? 'Switch to light theme' : 'Switch to dark theme',
            icon: isDark() ? 'light_mode' : 'dark_mode',
            onclick: () => toggleTheme(),
          }),
          m(MenuItem, {
            label: 'Keyboard shortcuts',
            icon: 'help_outline',
            onclick: () => toggleHelp(),
          }),
        ),
      ]),
    ]);
  }
}

// The BigTrace version you run, next to the brand. It opens a menu of the
// versions the page deploys, so there's nothing to show when that's just one.
function renderChannelPicker(): m.Children {
  const channels = getDeployedReleaseChannels();
  if (channels.length < 2) return null;
  const current = getReleaseChannel();
  return m(
    PopupMenu,
    {
      trigger: m(Chip, {
        label: [current, m(Icon, {icon: 'expand_more'})],
        intent: CHANNELS[current].intent,
        className: `pf-bt-topbar-channel pf-bt-topbar-channel--${current}`,
        interactive: true,
        title: 'BigTrace version',
      }),
      position: PopupPosition.BottomStart,
    },
    m(MenuTitle, {label: 'BigTrace version'}),
    channels.map((channel) =>
      m(MenuItem, {
        label: [
          CHANNELS[channel].label,
          m('.pf-bt-channel-menu__desc', CHANNELS[channel].description),
        ],
        icon:
          channel === current
            ? 'radio_button_checked'
            : 'radio_button_unchecked',
        ...(channel === current
          ? {}
          : {
              title: `Reloads the ${channel} version of BigTrace, which keeps its own tabs.`,
              onclick: () => switchReleaseChannel(channel),
            }),
      }),
    ),
  );
}

function isDark(): boolean {
  return settingsStorage.get('theme')?.get() === 'dark';
}

function toggleTheme(): void {
  const theme = settingsStorage.get('theme');
  if (theme) theme.set(isDark() ? 'light' : 'dark');
}
