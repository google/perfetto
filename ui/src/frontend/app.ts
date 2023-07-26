// Copyright (C) 2023 The Android Open Source Project
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

import {Trash} from '../base/disposable';
import {findRef} from '../base/dom_utils';
import {Actions} from '../common/actions';
import {setTimestampFormat, TimestampFormat} from '../common/time';
import {raf} from '../core/raf_scheduler';

import {addTab} from './bottom_tab';
import {onClickCopy} from './clipboard';
import {CookieConsent} from './cookie_consent';
import {globals} from './globals';
import {fullscreenModalContainer} from './modal';
import {executeSearch} from './search_handler';
import {Sidebar} from './sidebar';
import {SqlTableTab} from './sql_table/tab';
import {SqlTables} from './sql_table/well_known_tables';
import {Topbar} from './topbar';
import {shareTrace} from './trace_attrs';
import {HotkeyConfig, HotkeyContext} from './widgets/hotkey_context';

function renderPermalink(): m.Children {
  const permalink = globals.state.permalink;
  if (!permalink.requestId || !permalink.hash) return null;
  const url = `${self.location.origin}/#!/?s=${permalink.hash}`;
  const linkProps = {title: 'Click to copy the URL', onclick: onClickCopy(url)};

  return m('.alert-permalink', [
    m('div', 'Permalink: ', m(`a[href=${url}]`, linkProps, url)),
    m('button',
      {
        onclick: () => globals.dispatch(Actions.clearPermalink({})),
      },
      m('i.material-icons.disallow-selection', 'close')),
  ]);
}

class Alerts implements m.ClassComponent {
  view() {
    return m('.alerts', renderPermalink());
  }
}

export class App implements m.ClassComponent {
  private trash = new Trash();

  private omniboxCommandMode = false;
  private omniboxCommandValue = '';
  private focusOmniboxNextRender = false;

  private hotkeys: HotkeyConfig[] = [
    {
      key: 'p',
      mods: ['Mod', 'Shift'],
      allowInEditable: true,
      callback:
          () => {
            this.omniboxCommandMode = true;
            this.focusOmniboxNextRender = true;
            raf.scheduleFullRedraw();
          },
    },
    {
      key: 'o',
      mods: ['Mod'],
      allowInEditable: true,
      callback:
          () => {
            this.omniboxCommandMode = false;
            this.omniboxCommandValue = '';
            this.focusOmniboxNextRender = true;
            globals.dispatch(Actions.setOmniboxMode({mode: 'COMMAND'}));
            raf.scheduleFullRedraw();
          },
    },
    {
      key: 's',
      mods: ['Mod'],
      allowInEditable: true,
      callback:
          () => {
            this.omniboxCommandMode = false;
            this.omniboxCommandValue = '';
            this.focusOmniboxNextRender = true;
            globals.dispatch(Actions.setOmniboxMode({mode: 'SEARCH'}));
            raf.scheduleFullRedraw();
          },
    },
    {
      key: 'b',
      mods: ['Mod'],
      allowInEditable: true,
      callback: () => globals.dispatch(Actions.toggleSidebar({})),
    },
  ];

  private cmds = [
    {
      id: 'perfetto.SetTimestampFormatTimecodes',
      name: 'Set timestamp format: Timecode',
      callback:
          () => {
            setTimestampFormat(TimestampFormat.Timecode);
            raf.scheduleFullRedraw();
          },
    },
    {
      id: 'perfetto.SetTimestampFormatSeconds',
      name: 'Set timestamp format: Seconds',
      callback:
          () => {
            setTimestampFormat(TimestampFormat.Seconds);
            raf.scheduleFullRedraw();
          },
    },
    {
      id: 'perfetto.SetTimestampFormatRaw',
      name: 'Set timestamp format: Raw',
      callback:
          () => {
            setTimestampFormat(TimestampFormat.Raw);
            raf.scheduleFullRedraw();
          },
    },
    {
      id: 'perfetto.SetTimestampFormatLocaleRaw',
      name: 'Set timestamp format: Raw (formatted)',
      callback:
          () => {
            setTimestampFormat(TimestampFormat.RawLocale);
            raf.scheduleFullRedraw();
          },
    },
    {
      id: 'perfetto.ShowSliceTable',
      name: 'Show slice table',
      callback:
          () => {
            addTab({
              kind: SqlTableTab.kind,
              config: {
                table: SqlTables.slice,
                displayName: 'slice',
              },
            });
          },
    },
    {
      id: 'perfetto.ToggleLeftSidebar',
      name: 'Toggle left sidebar',
      callback: () => globals.dispatch(Actions.toggleSidebar({})),
    },
    {
      id: 'perfetto.TogglePerformanceMetrics',
      name: 'Toggle performance metrics',
      callback:
          () => {
            globals.dispatch(Actions.togglePerfDebug({}));
          },
    },
    {
      id: 'perfetto.ShareTrace',
      name: 'Share trace',
      callback: shareTrace,
    },
    {
      id: 'perfetto.SearchNext',
      name: 'Go to next search result',
      callback:
          () => {
            executeSearch();
          },
    },
    {
      id: 'perfetto.SearchPrev',
      name: 'Go to previous search result',
      callback:
          () => {
            executeSearch(true);
          },
    },
    {
      id: 'perfetto.OpenCommandPalette',
      name: 'Open Command Palette',
      callback:
          () => {
            this.omniboxCommandMode = true;
            this.omniboxCommandValue = '';
            this.focusOmniboxNextRender = true;
            raf.scheduleFullRedraw();
          },
    },
    {
      id: 'perfetto.RunQuery',
      name: 'Run Query',
      callback:
          () => {
            globals.dispatch(Actions.setOmniboxMode({mode: 'COMMAND'}));
            this.focusOmniboxNextRender = true;
            raf.scheduleFullRedraw();
          },
    },
    {
      id: 'perfetto.Search',
      name: 'Search',
      callback:
          () => {
            this.omniboxCommandMode = false;
            this.omniboxCommandValue = '';
            this.focusOmniboxNextRender = true;
            globals.dispatch(Actions.setOmniboxMode({mode: 'SEARCH'}));
            raf.scheduleFullRedraw();
          },
    },
  ];

  commands() {
    return this.cmds;
  }

  view({children}: m.Vnode): m.Children {
    return m(
        HotkeyContext,
        {hotkeys: this.hotkeys},
        m(
            'main',
            m(Sidebar),
            m(Topbar, {
              commandMode: this.omniboxCommandMode,
              commandText: this.omniboxCommandValue,
              onCommandModeChange: (value) => {
                this.omniboxCommandMode = value;
                raf.scheduleFullRedraw();
              },
              onCommandValueChange: (value) => {
                this.omniboxCommandValue = value;
                raf.scheduleFullRedraw();
              },
            }),
            m(Alerts),
            children,
            m(CookieConsent),
            m(fullscreenModalContainer.mithrilComponent),
            globals.state.perfDebug && m('.perf-stats'),
            ),
    );
  }

  oncreate({dom}: m.VnodeDOM) {
    const unreg = globals.commandManager.registerCommandSource(this);
    this.trash.add(unreg);

    this.maybeFocusOmnibar(dom);
  }

  onupdate({dom}: m.VnodeDOM) {
    this.maybeFocusOmnibar(dom);
  }

  onremove(_: m.VnodeDOM) {
    this.trash.dispose();
  }

  private maybeFocusOmnibar(dom: Element) {
    if (this.focusOmniboxNextRender) {
      const el = findRef(dom, Topbar.OMNIBOX_INPUT_REF);
      if (el && el instanceof HTMLInputElement) {
        el.focus();
        el.select();
      }
      this.focusOmniboxNextRender = false;
    }
  }
}
