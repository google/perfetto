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
import type {
  Embedder,
  NavTab,
  Navigation,
} from '../../../ui/src/core/embedder/embedder';
import type {App} from '../../../ui/src/public/app';
import {SysmonHomePage} from './home_page';
import {sysmonState} from './state';
import SysmonPlugin, {SYSMON_PLUGIN_ID} from './sysmon_plugin';

const BUILTIN_PLUGINS = [
  'dev.perfetto.Llm',
  'dev.perfetto.LlmProtocolChromePrompt',
  'dev.perfetto.LlmProtocolGemini',
  'dev.perfetto.LlmProtocolOpenAi',
  'dev.perfetto.RecordTraceV2',
  'dev.perfetto.SettingsPage',
];

function buildSysmonNavTabs(app: App): ReadonlyArray<NavTab> {
  const route = app.getCurrentRoute();
  const onSysmon = route.page === '/sysmon';
  const tabs: NavTab[] = [
    {
      label: sysmonState.liveSession ? 'Live' : 'Live Device',
      page: '/sysmon',
      kind: sysmonState.liveSession ? 'live' : undefined,
      icon: sysmonState.liveSession ? undefined : 'usb',
      href: () => '#!/sysmon?view=live',
      isActive: () => onSysmon && sysmonState.activeView.kind === 'live',
    },
    ...sysmonState.snapshots.map(
      (snap): NavTab => ({
        label: snap.label,
        page: '/sysmon',
        kind: 'snapshot',
        icon: snap.source === 'live' ? 'photo_camera' : 'description',
        href: () => `#!/sysmon?view=${encodeURIComponent(snap.id)}`,
        isActive: () =>
          onSysmon &&
          sysmonState.activeView.kind === 'snapshot' &&
          sysmonState.activeView.id === snap.id,
        onClose: () => sysmonState.closeSnapshot(snap.id),
      }),
    ),
  ];

  if (sysmonState.snapshots.length >= 2) {
    tabs.push({
      label: 'Compare',
      page: '/sysmon',
      kind: 'compare',
      icon: 'compare_arrows',
      href: () => '#!/sysmon?view=compare',
      isActive: () => onSysmon && sysmonState.activeView.kind === 'compare',
    });
  }
  return tabs;
}

export class SysmonEmbedder implements Embedder {
  readonly productId = 'sysmon';
  readonly appTitle = 'Sysmon';
  readonly analyticsId = undefined;
  readonly extensionServer = undefined;
  readonly brandingBadge = undefined;
  readonly defaultPlugins = [...BUILTIN_PLUGINS, SYSMON_PLUGIN_ID];
  readonly pluginAllowlist = BUILTIN_PLUGINS;
  readonly extraPlugins = [SysmonPlugin];
  readonly renderHomePage = () => m(SysmonHomePage);
  readonly navigation: Navigation = {
    kind: 'tabs',
    brand: {text: 'Sysmon', icon: 'monitoring'},
    tabs: buildSysmonNavTabs,
  };
}
