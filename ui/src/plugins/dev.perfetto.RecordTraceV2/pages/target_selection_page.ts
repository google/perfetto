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
import {RecordingTarget} from '../interfaces/recording_target';
import {SegmentedButtons} from '../../../widgets/segmented_buttons';
import {TARGET_PLATFORMS} from '../interfaces/target_platform';
import {RecordingTargetProvider} from '../interfaces/recording_target_provider';
import {Icon} from '../../../widgets/icon';
import {Button, ButtonBar, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {getOrCreate} from '../../../base/utils';
import {PreflightCheckRenderer} from './preflight_check_renderer';
import {Select} from '../../../widgets/select';
import {DisposableStack} from '../../../base/disposable_stack';
import {CurrentTracingSession, RecordingManager} from '../recording_manager';
import {downloadData} from '../../../base/download_utils';
import {RecordSubpage} from '../config/config_interfaces';
import {RecordPluginSchema} from '../serialization_schema';
import {Checkbox} from '../../../widgets/checkbox';
import {linkify} from '../../../base/mithril_utils';

type RecMgrAttrs = {recMgr: RecordingManager};

export function targetSelectionPage(recMgr: RecordingManager): RecordSubpage {
  return {
    kind: 'GLOBAL_PAGE',
    id: 'target',
    icon: 'cable',
    title: 'Target device',
    subtitle: 'Live recording via USB/WebSocket',
    render() {
      return m(TargetSelectionPage, {recMgr});
    },
    serialize(state: RecordPluginSchema) {
      state.target = {
        platformId: recMgr.currentPlatform,
        transportId: recMgr.currentProvider?.id,
        targetId: recMgr.currentTarget?.id,
      };
      state.autoOpenTrace = recMgr.autoOpenTraceWhenTracingEnds;
    },
    async deserialize(state: RecordPluginSchema) {
      recMgr.autoOpenTraceWhenTracingEnds = state.autoOpenTrace;
      if (state.target.platformId === undefined) return;
      recMgr.setPlatform(state.target.platformId);
      const prov = recMgr.getProvider(state.target.transportId ?? '');
      if (prov === undefined) return;
      await recMgr.setProvider(prov);
      if (state.target.targetId === undefined) return;
      for (const target of await recMgr.listTargets()) {
        if (target.id === state.target.targetId) {
          await recMgr.setTarget(target);
        }
      }
    },
  };
}

class TargetSelectionPage implements m.ClassComponent<RecMgrAttrs> {
  view({attrs}: m.CVnode<RecMgrAttrs>) {
    return [
      m('header', 'Select platform'),
      m(SegmentedButtons, {
        className: 'platform-selector',
        options: TARGET_PLATFORMS.map((p) => ({label: p.name, icon: p.icon})),
        selectedOption: TARGET_PLATFORMS.findIndex(
          (p) => p.id === attrs.recMgr.currentPlatform,
        ),
        onOptionSelected: (num) => {
          attrs.recMgr.setPlatform(TARGET_PLATFORMS[num].id);
          // m.redraw();
        },
      }),
      [
        m(TransportSelector, {
          recMgr: attrs.recMgr,
          key: attrs.recMgr.currentPlatform,
        }),
      ],
    ];
  }
}

class TransportSelector implements m.ClassComponent<RecMgrAttrs> {
  private transportKeys = new ObjToId();

  view({attrs}: m.CVnode<RecMgrAttrs>) {
    const options = [];
    for (const provider of attrs.recMgr.listProvidersForCurrentPlatform()) {
      const id = this.transportKeys.getKey(provider);
      options.push([
        m(`input[type=radio][name=recordingProvider][id=${id}]`, {
          onchange: async () => {
            await attrs.recMgr.setProvider(provider);
            m.redraw();
          },
          checked: attrs.recMgr.currentProvider === provider,
        }),
        m(
          `label[for=${id}]`,
          m(Icon, {icon: provider.icon}),
          m('.title', provider.name),
          m('.description', linkify(provider.description)),
        ),
      ]);
    }
    return [
      m('header', 'Select transport'),
      m('fieldset.record-transports', ...options),
      attrs.recMgr.currentProvider && [
        m(TargetSelector, {
          recMgr: attrs.recMgr,
          provider: attrs.recMgr.currentProvider,
          key: this.transportKeys.getKey(attrs.recMgr.currentProvider),
        }),
      ],
    ];
  }
}

type TargetSelectorAttrs = {
  recMgr: RecordingManager;
  provider: RecordingTargetProvider;
};
class TargetSelector implements m.ClassComponent<TargetSelectorAttrs> {
  private targetIdMap = new ObjToId();
  private checksRenderer: PreflightCheckRenderer;
  private trash = new DisposableStack();
  private targets: RecordingTarget[] = [];
  private provider: RecordingTargetProvider;
  private recMgr: RecordingManager;

  constructor({attrs}: m.CVnode<TargetSelectorAttrs>) {
    this.recMgr = attrs.recMgr;
    this.provider = attrs.provider;
    this.checksRenderer = new PreflightCheckRenderer(attrs.provider);
    this.trash.use(
      attrs.provider.onTargetsChanged.addListener(() => this.refreshTargets()),
    );
    this.checksRenderer
      .runPreflightChecks() //
      .then(() => this.refreshTargets());
    this.recMgr.listTargets().then((targets) => {
      this.targets = targets;
      m.redraw();
    });
  }

  view({attrs}: m.CVnode<TargetSelectorAttrs>) {
    const recMgr = attrs.recMgr;
    return [
      this.checksRenderer.renderTable(),
      m('header', 'Select target device'),

      m(
        '.record-targets',
        m(
          Select,
          {
            onchange: (e: Event) => {
              const idx = (e.target as HTMLSelectElement).selectedIndex;
              recMgr.setTarget(this.targets[idx]);
              // m.redraw();
            },
          },
          ...this.targets.map((target) =>
            m(
              'option',
              {selected: recMgr.currentTarget === target},
              target.name,
            ),
          ),
        ),
        m(Button, {
          icon: 'refresh',
          title: 'Refresh devices',
          onclick: () => {
            // This forces the TargetDetails component to be re-initialized,
            // in turn causing the pre-flight checks to be repeated. UX-wise
            // we want the refresh button to both reload the target list and
            // also reload the current target.
            this.targetIdMap.clear();
            this.refreshTargets();
          },
        }),
        recMgr.currentTarget &&
          m(Button, {
            icon: recMgr.currentTarget.connected ? 'cancel' : 'power_off',
            iconFilled: true,
            disabled: !recMgr.currentTarget.connected,
            title: recMgr.currentTarget.connected
              ? 'Disconnect the current device'
              : 'Device disconnected',
            onclick: () => recMgr.currentTarget?.disconnect(),
          }),
        attrs.provider.pairNewTarget &&
          m(Button, {
            label: 'Connect new device',
            icon: 'add',
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            onclick: async () => {
              const target = await attrs.provider.pairNewTarget!();
              target && recMgr.setTarget(target);
              await this.refreshTargets();
            },
          }),
      ),
      recMgr.currentTarget && [
        m(TargetDetails, {
          recMgr: attrs.recMgr,
          target: recMgr.currentTarget,
          key: this.targetIdMap.getKey(recMgr.currentTarget),
        }),
      ],
    ];
  }

  onremove() {
    this.trash.dispose();
  }

  private async refreshTargets() {
    // Re-triggers refresh and auto-select first valid target.
    this.recMgr.setProvider(this.provider);
    this.targets = await this.recMgr.listTargets();
    m.redraw();
  }
}

type TargetDetailsAttrs = {recMgr: RecordingManager; target: RecordingTarget};
class TargetDetails implements m.ClassComponent<TargetDetailsAttrs> {
  private checksRenderer?: PreflightCheckRenderer;

  constructor({attrs}: m.CVnode<TargetDetailsAttrs>) {
    this.checksRenderer = new PreflightCheckRenderer(attrs.target);
    this.checksRenderer.runPreflightChecks();
  }

  view({attrs}: m.CVnode<TargetDetailsAttrs>) {
    return [
      this.checksRenderer?.renderTable(),
      m(SessionMgmtRenderer, {recMgr: attrs.recMgr, target: attrs.target}),
    ];
  }
}

type SessionMgmtAttrs = {recMgr: RecordingManager; target: RecordingTarget};
class SessionMgmtRenderer implements m.ClassComponent<SessionMgmtAttrs> {
  view({attrs}: m.CVnode<SessionMgmtAttrs>) {
    const session = attrs.recMgr.currentSession;
    const isRecording = session?.state === 'RECORDING';
    return [
      m('header', 'Tracing session'),
      m(
        ButtonBar,
        m(Button, {
          label: 'Start tracing',
          icon: 'not_started',
          iconFilled: true,
          className: 'start',
          disabled: isRecording,
          onclick: () => attrs.recMgr.startTracing().then(() => m.redraw()),
        }),
        m(Button, {
          label: 'Stop',
          icon: 'stop',
          className: 'stop',
          iconFilled: true,
          disabled: !isRecording,
          onclick: () => session?.session?.stop().then(() => m.redraw()),
        }),
        m(Button, {
          label: 'Cancel',
          icon: 'cancel',
          className: 'cancel',
          iconFilled: true,
          disabled: !isRecording,
          onclick: () => session?.session?.cancel().then(() => m.redraw()),
        }),
        m(Checkbox, {
          label: 'Open trace when done',
          checked: attrs.recMgr.autoOpenTraceWhenTracingEnds,
          onchange: (e) => {
            attrs.recMgr.autoOpenTraceWhenTracingEnds = Boolean(
              (e.target as HTMLInputElement).checked,
            );
          },
        }),
      ),
      session?.error && m('div', session.error),
      session && [
        m(SessionStateRenderer, {
          session,
          key: session.uuid,
        }),
      ],
    ];
  }
}

type SessionStateAttrs = {
  session: CurrentTracingSession;
};
class SessionStateRenderer implements m.ClassComponent<SessionStateAttrs> {
  private session: CurrentTracingSession;
  private trash = new DisposableStack();
  private bufferUsagePct = 'N/A';

  constructor({attrs}: m.CVnode<SessionStateAttrs>) {
    this.session = attrs.session;
    this.trash.use(this.pollBufferState());
  }

  private pollBufferState(): Disposable {
    const timerId = window.setInterval(async () => {
      const bufferUsagePct = await this.session.session?.getBufferUsagePct();
      if (bufferUsagePct !== undefined) {
        // Retain the last valid buffer usage in the dialog, so the user can
        // get a sense of overruns even after the trace ends.
        this.bufferUsagePct = `${bufferUsagePct} %`;
      }
      m.redraw();
    }, 1000);
    return {
      [Symbol.dispose]() {
        window.clearInterval(timerId);
      },
    };
  }

  view() {
    const traceData = this.session.isCompleted
      ? this.session.session?.getTraceData()
      : undefined;
    const logs = this.getLogs();
    const eta = this.session.eta;
    return m(
      'table.session-status',
      m('tr', m('td', 'State'), m('td', this.session.state)),
      m('tr', m('td', 'Buffer usage'), m('td', this.bufferUsagePct)),
      eta && m('tr', m('td', 'ETA'), m('td', eta)),
      traceData &&
        m(
          'tr',
          m('td', 'Trace file'),
          m(
            'td',
            `${Math.round(traceData.length / 1e3).toLocaleString()} KB`,
            this.session.isCompressed && ' (compressed)',
            m(Button, {
              label: 'Open',
              icon: 'file_open',
              onclick: () => this.session.openTrace(),
            }),
            m(Button, {
              label: 'Download',
              icon: 'download',
              onclick: () => downloadData(this.session.fileName, traceData),
            }),
          ),
        ),
      logs != '' && m('tr', m('td', 'Logs'), m('td', m('pre.logs', logs))),
    );
  }

  onremove() {
    this.trash.dispose();
  }

  private getLogs() {
    let log = '';
    for (const l of this.session.session?.logs ?? []) {
      const timestamp = l.timestamp.toTimeString().substring(0, 8);
      log += `${timestamp}: ${l.message}\n`;
    }
    return log;
  }
}

/**
 * A utility class to assign unique string IDs to object instances.
 * This is used to generate the key: attr for mithril, for components that take
 * an object instance as attr, to ensure that mithril instantiates a new
 * component when the input object changes.
 * Example:
 * let obj = new MyFoo();
 * const map = new ObjId();
 * console.log(map.getKey(obj));  // Prints 'obj_1'.
 * console.log(map.getKey(obj));  // Prints 'obj_1'.
 * obj = new MyFoo();
 * console.log(map.getKey(obj));  // Prints 'obj_2'.
 */
export class ObjToId {
  private map = new WeakMap<object, string>();
  private lastId = 0;

  getKey(obj: object): string {
    return getOrCreate(this.map, obj, () => `obj_${++this.lastId}`);
  }

  clear() {
    this.map = new WeakMap<object, string>();
  }
}
