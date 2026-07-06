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

import './home_page.scss';
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
import {HotkeyGlyphs, Keycap} from '../widgets/hotkey_glyphs';
import {Switch} from '../widgets/switch';
import {assetSrc} from '../base/assets';
import {Stack} from '../widgets/stack';
import {Icons} from '../base/semantic_icons';
import {Icon} from '../widgets/icon';
import {classNames} from '../base/classnames';
import {Router} from '../core/router';
import {
  type KeyboardLayoutMap,
  nativeKeyboardLayoutMap,
  NotSupportedError,
} from '../base/keyboard_layout_map';
import {Spinner} from '../widgets/spinner';
import {KeyMapping} from '../base/wasd_key_mapping';
import {
  deleteCachedTrace,
  getRecentCachedTraces,
  type CachedTraceEntry,
} from '../core/cache_manager';
import {formatBytesSi} from '../base/bytes_format';

export class HomePage implements m.ClassComponent {
  view() {
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
        '.pf-channel-select__text',
        'Feeling adventurous? Try our bleeding edge Canary version.',
      ),
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

// A fallback keyboard map based on the QWERTY keymap. Converts keyboard event
// codes to their associated glyphs on an English QWERTY keyboard.
class EnglishQwertyKeyboardLayoutMap implements KeyboardLayoutMap {
  get(code: string): string {
    // Converts 'KeyX' -> 'x'
    return code.replace(/^Key([A-Z])$/, '$1').toLowerCase();
  }
}

class Hints implements m.ClassComponent {
  private keyMap?: KeyboardLayoutMap;

  oninit() {
    nativeKeyboardLayoutMap()
      .then((keyMap: KeyboardLayoutMap) => {
        this.keyMap = keyMap;
        m.redraw();
      })
      .catch((e) => {
        if (
          e instanceof NotSupportedError ||
          String(e).includes('SecurityError')
        ) {
          // Keyboard layout is unavailable. Fall back to English QWERTY.
          this.keyMap = new EnglishQwertyKeyboardLayoutMap();
          m.redraw();
        } else {
          throw e;
        }
      });
  }

  private codeToKeycap(code: string): m.Children {
    if (this.keyMap) {
      return m(Keycap, this.keyMap.get(code)?.toUpperCase());
    } else {
      return m(Keycap, m(Spinner));
    }
  }

  view() {
    const themeSetting = AppImpl.instance.settings.get<string>('theme');
    const isDarkMode = themeSetting?.get() === 'dark';

    return m(
      '.pf-home-page__hints',
      // Getting started section with Open/Record buttons
      m(
        '.pf-home-page__section',
        m('.pf-home-page__section-title', 'Quick start'),
        m(
          '.pf-home-page__section-content',
          m(
            Stack,
            m(
              '.pf-home-page__item',
              {
                onclick: () => {
                  app.commands.runCommand('dev.perfetto.OpenTrace');
                },
              },
              m(Icon, {icon: 'folder_open', className: 'pf-left-icon'}),
              m(
                '.pf-home-page__item-info',
                m('.pf-home-page__item-title', 'Open trace'),
              ),
            ),
            m(
              '.pf-home-page__item',
              {
                onclick: () => {
                  Router.navigate('#!/record');
                },
              },
              m(Icon, {icon: 'fiber_smart_record', className: 'pf-left-icon'}),
              m(
                '.pf-home-page__item-info',
                m('.pf-home-page__item-title', 'Record new trace'),
              ),
            ),
          ),
        ),
      ),
      m(RecentTraces),
      // Keyboard shortcuts section
      m(
        '.pf-home-page__section',
        m('.pf-home-page__section-title', 'Shortcuts'),
        m(
          '.pf-home-page__section-content',
          m(
            '.pf-home-page__shortcut',
            m('span.pf-home-page__shortcut-label', 'Find tracks'),
            m(HotkeyGlyphs, {hotkey: 'Mod+P'}),
          ),
          m(
            '.pf-home-page__shortcut',
            m('span.pf-home-page__shortcut-label', 'Navigate timeline'),
            m(
              Stack,
              {inline: true, spacing: 'small', orientation: 'horizontal'},
              [
                this.codeToKeycap(KeyMapping.KEY_ZOOM_IN),
                this.codeToKeycap(KeyMapping.KEY_PAN_LEFT),
                this.codeToKeycap(KeyMapping.KEY_ZOOM_OUT),
                this.codeToKeycap(KeyMapping.KEY_PAN_RIGHT),
              ],
            ),
          ),
          m(
            '.pf-home-page__shortcut',
            m('span.pf-home-page__shortcut-label', 'Commands'),
            m(HotkeyGlyphs, {hotkey: '!Mod+Shift+P'}),
          ),
        ),
      ),
      // Centered links below the cards
      m(
        '.pf-home-page__links',
        m(
          Anchor,
          {
            href: 'https://perfetto.dev/docs/visualization/perfetto-ui',
            icon: Icons.ExternalLink,
            target: '_blank',
          },
          'Getting started',
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
    );
  }
}

function RecentTraces(): m.Component {
  let recentTraces: ReadonlyArray<CachedTraceEntry> = [];
  let loading = true;
  const MAX_RECENT_TRACES = 5;

  function reloadTraces() {
    getRecentCachedTraces().then((traces) => {
      recentTraces = traces.slice(0, MAX_RECENT_TRACES);
      loading = false;
      m.redraw();
    });
  }

  reloadTraces();

  return {
    view() {
      if (loading || recentTraces.length === 0) {
        return null;
      }

      return m(
        '.pf-home-page__section',
        m('.pf-home-page__section-title', 'Recently opened traces'),
        m(
          '.pf-home-page__section-content',
          m(
            Stack,
            recentTraces.map((trace) => {
              return m(
                '.pf-home-page__item',
                {
                  onclick: () => {
                    Router.navigate(`#!/?local_cache_key=${trace.uuid}`);
                  },
                },
                m(Icon, {icon: 'description', className: 'pf-left-icon'}),
                m(
                  '.pf-home-page__item-info',
                  m('.pf-home-page__item-title', trace.title),
                  m(
                    '.pf-home-page__item-details',
                    trace.sizeBytes !== undefined &&
                      formatBytesSi(trace.sizeBytes),
                  ),
                ),
                m(Button, {
                  className: 'pf-visible-on-hover',
                  icon: 'close',
                  variant: ButtonVariant.Minimal,
                  title: 'Remove from cache',
                  onclick: async (e: Event) => {
                    e.stopPropagation();
                    if (
                      window.confirm(
                        'Are you sure you want to remove this trace?',
                      )
                    ) {
                      await deleteCachedTrace(trace.uuid);
                      reloadTraces();
                    }
                  },
                }),
              );
            }),
          ),
        ),
      );
    },
  };
}
