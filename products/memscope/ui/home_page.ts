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
import './home_page.scss';
import {AppImpl} from '../../../ui/src/core/app_impl';
import {Button, ButtonVariant} from '../../../ui/src/widgets/button';
import {Icon} from '../../../ui/src/widgets/icon';
import {Intent} from '../../../ui/src/widgets/common';
import {
  lastMemoryExplorerHref,
  traceHasMemoryExplorer,
} from '../../../ui/src/plugins/dev.perfetto.Memscope/memory_explorer/route';

const CLI_EXAMPLE = 'memscope record --app com.example.myapp';

export class MemscopeHomePage implements m.ClassComponent {
  view() {
    const app = AppImpl.instance;
    const trace = app.trace;
    return m(
      '.pf-memscope-home',
      m(
        '.pf-memscope-home__hero',
        m(Icon, {icon: 'memory', className: 'pf-memscope-home__logo'}),
        m('h1', 'Memscope'),
        m(
          'p.pf-memscope-home__tagline',
          "Find out where one app's memory goes, live, with an assistant " +
            'that can take heap dumps and explain them.',
        ),
      ),
      m(
        '.pf-memscope-home__cards',
        m(
          '.pf-memscope-home__card',
          m('h2', 'Profile an app live'),
          m(
            'p',
            'Connect an Android phone over USB or a Linux machine running ' +
              'traced, pick an app and watch its memory as you use it.',
          ),
          m(Button, {
            label: 'Connect a device',
            icon: 'usb',
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            onclick: () => app.navigate('#!/memscope'),
          }),
        ),
        m(
          '.pf-memscope-home__card',
          m('h2', 'Open a recording'),
          m(
            'p',
            'Open a trace with heap dumps, heap profiles or smaps snapshots, ' +
              'or record one from the command line:',
          ),
          m('code.pf-memscope-home__cli', CLI_EXAMPLE),
          m(Button, {
            label: 'Open trace file',
            icon: 'folder_open',
            variant: ButtonVariant.Outlined,
            onclick: () => app.commands.runCommand('dev.perfetto.OpenTrace'),
          }),
        ),
      ),
      trace &&
        m(
          '.pf-memscope-home__current',
          m('span', `Loaded: ${trace.traceInfo.traceTitle}`),
          traceHasMemoryExplorer(trace.traceInfo.uuid) &&
            m(Button, {
              label: 'Open Memory Explorer',
              icon: 'arrow_forward',
              intent: Intent.Primary,
              onclick: () =>
                app.navigate(lastMemoryExplorerHref(trace.traceInfo.uuid)),
            }),
          m(Button, {
            label: 'Open Timeline',
            icon: 'timeline',
            variant: traceHasMemoryExplorer(trace.traceInfo.uuid)
              ? ButtonVariant.Outlined
              : ButtonVariant.Filled,
            intent: traceHasMemoryExplorer(trace.traceInfo.uuid)
              ? Intent.None
              : Intent.Primary,
            onclick: () => app.navigate('#!/viewer'),
          }),
        ),
    );
  }
}
