// Copyright (C) 2018 The Android Open Source Project
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
import {
  channelChanged,
  getCurrentChannel,
  getNextChannel,
  setChannel,
} from '../core/channels';
import {AppImpl} from '../core/app_impl';
import {Anchor} from '../widgets/anchor';
import {Button, ButtonVariant} from '../widgets/button';
import {Intent} from '../widgets/common';
import {assetSrc} from '../base/assets';
import {Icons} from '../base/semantic_icons';
import {Icon} from '../widgets/icon';
import {HotkeyGlyphs} from '../widgets/hotkey_glyphs';
import {classNames} from '../base/classnames';
import {MenuItem, PopupMenu} from '../widgets/menu';
import {PopupPosition} from '../widgets/popup';
import {Switch} from '../widgets/switch';

export class HomePage implements m.ClassComponent {
  view() {
    const themeSetting = AppImpl.instance.settings.get<string>('theme');
    const isDarkMode = themeSetting?.get() === 'dark';

    return m(
      '.pf-home-page',
      m(
        '.pf-home-page__center',
        m(
          '.pf-home-page__title',
          m(`img.logo[src=${assetSrc('assets/logo-3d.png')}]`),
          'Perfetto',
        ),
        m(Hints),
        m(
          '.pf-home-page__links',
          m(
            Anchor,
            {
              href: 'https://perfetto.dev/docs/visualization/perfetto-ui',
              icon: Icons.ExternalLink,
              target: '_blank',
            },
            'Read the docs',
          ),
          m('.pf-home-page__links-separator'),
          m(Switch, {
            label: 'Dark mode',
            checked: isDarkMode,
            onchange: (e) => {
              themeSetting?.set(
                (e.target as HTMLInputElement).checked ? 'dark' : 'light',
              );
            },
          }),
        ),
        m(ChannelSelect),
      ),
      m(
        Anchor,
        {
          className: 'pf-home-page__privacy',
          href: 'https://policies.google.com/privacy',
          target: '_blank',
          icon: Icons.ExternalLink,
        },
        'Privacy policy',
      ),
    );
  }
}

class ChannelSelect implements m.ClassComponent {
  view() {
    const showAutopush = getCurrentChannel() === 'autopush';
    const channels = showAutopush
      ? ['stable', 'canary', 'autopush']
      : ['stable', 'canary'];

    return m(
      `.pf-channel-select`,
      {
        className: classNames(
          showAutopush && 'pf-channel-select--with-autopush',
        ),
      },
      m(
        'fieldset.pf-channel-select__switch',
        ...channels.map((channel) => {
          const checked = getNextChannel() === channel ? '[checked=true]' : '';
          return [
            m(`input[type=radio][name=chan][id=chan_${channel}]${checked}`, {
              onchange: () => {
                setChannel(channel);
              },
            }),
            m(`label[for=chan_${channel}]`, channel),
          ];
        }),
        m('.pf-channel-select__pill'),
      ),
      m(
        '.pf-channel-select__reload-hint',
        {
          className: classNames(
            channelChanged() && 'pf-channel-select__reload-hint--visible',
          ),
        },
        m(
          '.pf-channel-select__reload-hint-text',
          'You need to reload the page for the changes to have effect',
        ),
        m(Button, {
          label: 'Reload',
          icon: 'refresh',
          variant: ButtonVariant.Filled,
          intent: Intent.Danger,
          onclick: () => window.location.reload(),
        }),
      ),
    );
  }
}

interface QuickStartButton {
  readonly commandId: string;
  readonly icon: string;
  readonly label: string;
}

interface QuickStartDropdown {
  readonly icon: string;
  readonly label: string;
  readonly items: ReadonlyArray<{
    readonly commandId: string;
    readonly icon: string;
    readonly label: string;
  }>;
}

type QuickStartEntry = QuickStartButton | QuickStartDropdown;

function isDropdown(e: QuickStartEntry): e is QuickStartDropdown {
  return 'items' in e;
}

const QUICK_START_ENTRIES: QuickStartEntry[] = [
  {
    commandId: 'dev.perfetto.OpenTrace',
    icon: 'folder_open',
    label: 'Open trace',
  },
  {
    commandId: 'dev.perfetto.RecordTrace',
    icon: 'fiber_smart_record',
    label: 'Record new trace',
  },
  {
    icon: 'science',
    label: 'Open example trace',
    items: [
      {
        commandId: 'dev.perfetto.OpenExampleAndroidTrace',
        icon: 'android',
        label: 'Android example',
      },
      {
        commandId: 'dev.perfetto.OpenExampleChromeTrace',
        icon: 'web',
        label: 'Chrome example',
      },
    ],
  },
  {
    commandId: 'dev.perfetto.OpenCommandPalette',
    icon: 'terminal',
    label: 'Command palette',
  },
];

function renderQuickStartButton(entry: QuickStartButton): m.Children {
  const cmds = AppImpl.instance.commands;
  if (!cmds.hasCommand(entry.commandId)) return null;
  const cmd = cmds.getCommand(entry.commandId);
  return m(
    '.pf-home-page__button',
    {onclick: () => cmds.runCommand(entry.commandId)},
    m(Icon, {icon: entry.icon, className: 'pf-left-icon'}),
    m('span.pf-button__label', entry.label),
    cmd.defaultHotkey &&
      m(HotkeyGlyphs, {className: 'pf-right', hotkey: cmd.defaultHotkey}),
  );
}

function renderQuickStartDropdown(entry: QuickStartDropdown): m.Children {
  const cmds = AppImpl.instance.commands;
  const visibleItems = entry.items.filter((i) => cmds.hasCommand(i.commandId));
  if (visibleItems.length === 0) return null;
  return m(
    PopupMenu,
    {
      trigger: m(
        '.pf-home-page__button',
        m(Icon, {icon: entry.icon, className: 'pf-left-icon'}),
        m('span.pf-button__label', entry.label),
        m(Icon, {className: 'pf-right', icon: 'chevron_right'}),
      ),
      position: PopupPosition.RightStart,
    },
    ...visibleItems.map((item) =>
      m(MenuItem, {
        label: item.label,
        icon: item.icon,
        onclick: () => cmds.runCommand(item.commandId),
      }),
    ),
  );
}

class Hints implements m.ClassComponent {
  view() {
    return m(
      '.pf-home-page__hints',
      m(
        '.pf-home-page__section',
        m('.pf-home-page__section-title', 'Quick start'),
        m(
          '.pf-home-page__section-content',
          m(
            '.pf-home-page__getting-started-buttons',
            ...QUICK_START_ENTRIES.map((entry) =>
              isDropdown(entry)
                ? renderQuickStartDropdown(entry)
                : renderQuickStartButton(entry),
            ),
          ),
        ),
      ),
    );
  }
}
