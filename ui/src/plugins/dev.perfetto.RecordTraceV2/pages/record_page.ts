// Copyright (C) 2024 The Android Open Source Project
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
import {RecordingManager} from '../recording_manager';
import {Icon} from '../../../widgets/icon';
import {RecordSubpage, supportsPlatform} from '../config/config_interfaces';
import {Probe} from './probe_renderer';
import {Button} from '../../../widgets/button';
import {classNames} from '../../../base/classnames';
import {showModal} from '../../../widgets/modal';
import {BUCKET_NAME} from '../../../base/gcs_uploader';
import {RecordingTarget} from '../interfaces/recording_target';
import {exists} from '../../../base/utils';
import {SHARE_SUBPAGE} from '../config/config_sharing';
import {App} from '../../../public/app';
import {Callout} from '../../../widgets/callout';
import {Intent} from '../../../widgets/common';
import {Icons} from '../../../base/semantic_icons';
import {Stack} from '../../../widgets/stack';
import {Anchor} from '../../../widgets/anchor';

export interface RecordPageAttrs {
  readonly app: App;
  readonly subpage?: string;
  readonly getRecordingManager: () => RecordingManager;
}

const DEFAULT_SUBPAGE = 'target';
const PERSIST_EVERY_MS = 1000;

// By design this interface overlaps with RecordConfigSection so we can use the
// same for custom subpages (record, config) and the probe settings.
interface MenuEntry {
  readonly id: string;
  readonly icon: string;
  readonly title: string;
  readonly subtitle: string;
}

export class RecordPageV2 implements m.ClassComponent<RecordPageAttrs> {
  private recMgr: RecordingManager;
  private subpage: string = DEFAULT_SUBPAGE;
  private persistTimer: number | undefined = undefined;

  constructor({attrs}: m.CVnode<RecordPageAttrs>) {
    this.recMgr = attrs.getRecordingManager();
    if (attrs.subpage && attrs.subpage.startsWith('/' + SHARE_SUBPAGE)) {
      this.loadShared(attrs.subpage.substring(SHARE_SUBPAGE.length + 2));
    }
  }

  view({attrs}: m.CVnode<RecordPageAttrs>) {
    if (this.persistTimer === undefined) {
      this.persistTimer = window.setTimeout(() => {
        this.recMgr.persistIntoLocalStorage();
        this.persistTimer = undefined;
      }, PERSIST_EVERY_MS);
    }
    this.subpage =
      exists(attrs.subpage) && attrs.subpage.length > 0
        ? attrs.subpage.substring(1)
        : DEFAULT_SUBPAGE;

    const cmdlineUrl =
      'https://perfetto.dev/docs/quickstart/android-tracing#perfetto-cmdline';
    return m(
      '.pf-record-page',
      m(
        Stack,
        {className: 'pf-record-page__container'},
        this.recMgr.recordConfig.traceConfig.mode === 'LONG_TRACE' &&
          m(
            Callout,
            {intent: Intent.Warning, icon: Icons.Warning},
            `
              Recording in long trace mode through the UI is not supported.
              Please copy the command and `,
            m(
              Anchor,
              {href: cmdlineUrl, target: '_blank'},
              `collect the trace using ADB.`,
            ),
          ),
        m(
          '.pf-record-page__container-content',
          this.renderMenu(), //
          this.renderSubPage(), //
        ),
      ),
    );
  }

  onremove() {
    window.clearTimeout(this.persistTimer);
    this.recMgr.persistIntoLocalStorage();
  }

  private renderSubPage(): m.Children {
    const page = this.recMgr.pages.get(this.subpage);
    if (page === undefined) {
      return m(
        '.pf-record-page__section.active',
        m('header', `Invalid subpage /record/${this.subpage}`),
      );
    }
    return [
      m(
        '.pf-record-page__section.active',
        {id: page.id, key: page.id},
        this.renderSubpage(page),
      ),
    ];
  }

  private renderSubpage(page: RecordSubpage): m.Children {
    switch (page.kind) {
      case 'PROBES_PAGE':
        return page.probes
          .filter((p) => supportsPlatform(p, this.recMgr.currentPlatform))
          .map((probe) => m(Probe, {cfgMgr: this.recMgr.recordConfig, probe}));
      case 'GLOBAL_PAGE':
      case 'SESSION_PAGE':
        return page.render();
    }
  }

  private static readonly PROBE_ORDER: {[key: string]: number} = {
    cpu: 10,
    gpu: 20,
    power: 30,
    memory: 40,
    android: 50,
    network: 60,
    chrome: 70,
    stack_sampling: 80,
    perfetto_sdk: 90,
    advanced: 200,
  };

  private getSortedProbes(pages: RecordSubpage[]): RecordSubpage[] {
    const probes = pages.filter((p) => p.kind === 'PROBES_PAGE');

    const probesWithMeta = probes.map((probe) => {
      const availProbes =
        probe.kind === 'PROBES_PAGE'
          ? probe.probes.filter((p) =>
              supportsPlatform(p, this.recMgr.currentPlatform),
            ).length
          : 0;

      return {
        probe,
        disabled: availProbes === 0,
        order: RecordPageV2.PROBE_ORDER[probe.id] ?? 100,
      };
    });

    return probesWithMeta
      .sort((a, b) =>
        a.disabled !== b.disabled ? (a.disabled ? 1 : -1) : a.order - b.order,
      )
      .map(({probe}) => probe);
  }

  private renderMenu() {
    const pages = this.recMgr.pages;
    return m(
      '.pf-record-page__menu',
      m(RecordingCtl, {recMgr: this.recMgr}),
      m('header', 'Record'),
      m(
        'ul',
        this.renderMenuEntry(pages.get('target')), // Overview
        this.renderMenuEntry(pages.get('cmdline')),
      ),
      m('header', 'Recording settings'),
      m('ul', this.renderMenuEntry(pages.get('config'))),
      m(
        'header',
        'Probes',
        m(Button, {
          icon: 'delete_sweep',
          title: 'Clear current configuration',
          onclick: () => {
            if (confirm('The current config will be cleared. Are you sure?')) {
              this.recMgr.clearSession();
            }
          },
        }),
      ),
      m(
        'ul',
        this.getSortedProbes(Array.from(pages.values())).map((rc) =>
          this.renderMenuEntry(rc),
        ),
      ),
    );
  }

  private renderMenuEntry(rc: MenuEntry | undefined) {
    if (!rc) return null;
    let enabledProbes = 0;
    let availProbes = 0;
    let probeCountTxt = '';
    const probePage = this.recMgr.pages.get(rc.id);
    if (probePage?.kind === 'PROBES_PAGE') {
      for (const probe of probePage.probes) {
        if (!supportsPlatform(probe, this.recMgr.currentPlatform)) continue;
        ++availProbes;
        if (!this.recMgr.recordConfig.isProbeEnabled(probe.id)) continue;
        ++enabledProbes;
      }
      probeCountTxt = `${enabledProbes > 0 ? enabledProbes : ''}`;
    }
    const disabled = availProbes === 0 && probePage?.kind === 'PROBES_PAGE';
    const className = classNames(
      this.subpage === rc.id && 'active',
      disabled && 'disabled',
    );
    return m(
      'a',
      {href: disabled ? undefined : `#!/record/${rc.id}`},
      m(
        'li',
        {className},
        m(Icon, {icon: rc.icon}),
        m('.title', rc.title, m('.probe-count', probeCountTxt)),
        m('.sub', rc.subtitle),
      ),
    );
  }

  private async loadShared(hash: string) {
    const url = `https://storage.googleapis.com/${BUCKET_NAME}/${hash}`;
    const fetchData = await fetch(url);
    const json = await fetchData.text();
    const res = this.recMgr.restoreSessionFromJson(json);
    if (!res.ok) {
      showModal({title: 'Restore error', content: res.error});
      return;
    }
    this.recMgr.app.navigate('#!/record/cmdline');
  }
}

interface RecCtlAttrs {
  recMgr: RecordingManager;
}

class RecordingCtl implements m.ClassComponent<RecCtlAttrs> {
  private recMgr: RecordingManager;
  private lastTarget?: RecordingTarget;

  constructor({attrs}: m.CVnode<RecCtlAttrs>) {
    this.recMgr = attrs.recMgr;
  }

  view() {
    const target = this.recMgr.currentTarget;
    if (this.lastTarget !== target) {
      this.lastTarget = target;
    }

    const currentSession = this.recMgr.currentSession;
    const recordingInProgress = currentSession?.inProgress;
    if (recordingInProgress) {
      // Update the ETA if the recording is in progress.
      setTimeout(() => m.redraw(), 1000);
    }
    const eta: string | undefined = currentSession?.eta;
    return m(
      '.record-ctl',
      m(Button, {
        icon: 'cable',
        title: 'Click to select another target',
        onclick: () => this.recMgr.app.navigate('#!/record/target'),
      }),
      m(
        '.record-target',
        recordingInProgress
          ? `Recording${eta ? ', ETA ' + eta : ''}`
          : target?.name ?? 'No target selected',
      ),
      recordingInProgress
        ? m(Button, {
            icon: 'stop',
            disabled: currentSession.state !== 'RECORDING',
            iconFilled: true,
            title: 'Stop',
            className: 'rec',
            onclick: () => {
              currentSession.session?.stop();
              this.recMgr.app.navigate('#!/record/target');
            },
          })
        : m(Button, {
            icon: 'not_started',
            disabled:
              target === undefined ||
              this.recMgr.recordConfig.traceConfig.mode === 'LONG_TRACE',
            iconFilled: true,
            title: 'Start tracing',
            className: 'rec',
            onclick: () => {
              this.recMgr.startTracing();
              this.recMgr.app.navigate('#!/record/target');
            },
          }),
    );
  }
}
