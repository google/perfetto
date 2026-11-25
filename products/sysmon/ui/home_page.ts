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

import './home_page.scss';
import m from 'mithril';
import {AppImpl} from '../../../ui/src/core/app_impl';
import {Button, ButtonVariant} from '../../../ui/src/widgets/button';
import {Callout} from '../../../ui/src/widgets/callout';
import {Intent} from '../../../ui/src/widgets/common';
import {Icon} from '../../../ui/src/widgets/icon';
import {Spinner} from '../../../ui/src/widgets/spinner';
import {sysmonState} from './state';

// Demo traces served relative to the app root so they work identically on local
// dev servers and subpath static hosts.
const DEMO_TRACES = [
  {
    label: 'Android Post-Boot Unlock',
    subtitle: 'CPU, thread waits, memory, DMA-BUF & GPU · 17.8 MB',
    url: './test/data/android_postboot_unlock.pftrace',
  },
  {
    label: 'Android Boot Trace',
    subtitle: 'Multi-core CPU contention & boot scheduling · 12.5 MB',
    url: './test/data/android_boot.pftrace',
  },
];

// Clean landing page modeled on lat_home_page.ts: simple hero, 2 numbered
// action steps, and sample trace cards.
export function SysmonHomePage(): m.Component {
  const triggerFilePicker = () => {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.pftrace,.perfetto-trace,.pb,.gz';
    input.onchange = async () => {
      const f = input.files?.[0];
      if (!f) return;
      const buf = new Uint8Array(await f.arrayBuffer());
      await sysmonState.openTraceBytes(buf, f.name);
    };
    input.click();
  };

  return {
    view() {
      const app = AppImpl.instance;
      return m('main.pf-sysmon-home', [
        m('header.pf-sysmon-home__hero', [
          m('.pf-sysmon-home__brand', [
            m(Icon, {icon: 'monitoring'}),
            m('span', 'Sysmon'),
          ]),
          m('h1', 'What is slowing the device down?'),
          m(
            'p.pf-sysmon-home__lede',
            'Monitor whole-device CPU, memory, storage, and thermal behavior live, or open a Perfetto trace.',
          ),
        ]),

        m('section.pf-sysmon-home__steps', [
          m('article.pf-sysmon-home__step', [
            m('span.pf-sysmon-home__number', '1'),
            m('h2', 'Monitor live'),
            m(
              'p',
              'Connect an Android or Linux device to watch system vitals and capture snapshots.',
            ),
            m(Button, {
              label: sysmonState.liveSession
                ? `Return to Live (${sysmonState.liveSession.deviceName})`
                : 'Connect a device',
              icon: 'usb',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
              onclick: () => app.navigate('#!/sysmon?view=live'),
            }),
          ]),
          m('article.pf-sysmon-home__step', [
            m('span.pf-sysmon-home__number', '2'),
            m('h2', 'Open a trace'),
            m('p', 'Inspect any Perfetto trace locally. Nothing leaves your machine.'),
            m(Button, {
              label: 'Open trace file',
              icon: 'folder_open',
              variant: ButtonVariant.Outlined,
              onclick: triggerFilePicker,
            }),
          ]),
        ]),

        sysmonState.loadingTraceLabel &&
          m('.pf-sysmon-home__loading', [
            m(Spinner, {easing: true}),
            m('span', `Loading ${sysmonState.loadingTraceLabel}…`),
          ]),

        sysmonState.lastError &&
          m(Callout, {icon: 'error', intent: Intent.Danger}, sysmonState.lastError),

        m('section.pf-sysmon-home__demos', [
          m('h2', 'Sample traces'),
          m(
            '.pf-sysmon-home__demo-grid',
            DEMO_TRACES.map((d) =>
              m(
                'button.pf-sysmon-home__demo-card',
                {
                  disabled: sysmonState.loadingTraceLabel !== undefined,
                  onclick: () => void sysmonState.openTraceUrl(d.url, d.label),
                },
                [
                  m('.pf-sysmon-home__demo-name', d.label),
                  m('.pf-sysmon-home__demo-sub', d.subtitle),
                ],
              ),
            ),
          ),
        ]),
      ]);
    },
  };
}
