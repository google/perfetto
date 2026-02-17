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
import {download} from '../../../base/download_utils';
import {RecordSubpage} from '../config/config_interfaces';
import {RecordPluginSchema, SavedSessionSchema} from '../serialization_schema';
import {Checkbox} from '../../../widgets/checkbox';
import {linkify} from '../../../widgets/anchor';
import {getPresetsForPlatform} from '../presets';
import {Icons} from '../../../base/semantic_icons';
import {shareRecordConfig} from '../config/config_sharing';
import {Card} from '../../../widgets/card';
import {showModal} from '../../../widgets/modal';
import {traceConfigToPb} from '../../../base/proto_utils_wasm';
import protos from '../../../protos';

type RecMgrAttrs = {recMgr: RecordingManager};

export function targetSelectionPage(recMgr: RecordingManager): RecordSubpage {
  return {
    kind: 'GLOBAL_PAGE',
    id: 'target',
    icon: 'dashboard',
    title: 'Overview',
    subtitle: 'Start a new trace',
    render() {
      return m(OverviewPage, {recMgr});
    },
    serialize(state: RecordPluginSchema) {
      state.target = {
        platformId: recMgr.currentPlatform,
        transportId: recMgr.currentProvider?.id,
        targetId: recMgr.currentTarget?.id,
      };
      state.autoOpenTrace = recMgr.autoOpenTraceWhenTracingEnds;
      state.selectedConfigId = recMgr.selectedConfigId;
      state.configModified = recMgr.isConfigModified;
    },
    async deserialize(state: RecordPluginSchema) {
      recMgr.autoOpenTraceWhenTracingEnds = state.autoOpenTrace;

      // Restore platform selection
      if (state.target.platformId !== undefined) {
        recMgr.setPlatform(state.target.platformId);
      }

      // Restore config
      const hasSavedProbes =
        state.lastSession !== undefined &&
        state.lastSession.kind === 'probes' &&
        Object.keys(state.lastSession.probes).length > 0;

      if (state.selectedConfigId || hasSavedProbes) {
        if (state.selectedConfigId) {
          recMgr.loadConfig({
            config: state.lastSession,
            configId: state.selectedConfigId,
            configName: recMgr.resolveConfigName(state.selectedConfigId),
            configModified: state.configModified,
          });
        } else {
          recMgr.loadSession(state.lastSession);
        }
      } else {
        recMgr.loadDefaultConfig();
      }

      // Restore provider selection
      const prov = recMgr.getProvider(state.target.transportId ?? '');
      if (prov !== undefined) {
        await recMgr.setProvider(prov);
      }

      // Restore target selection
      if (state.target.targetId !== undefined) {
        const targets = await recMgr.listTargets();
        const target = targets.find((t) => t.id === state.target.targetId);
        if (target) {
          recMgr.setTarget(target);
        }
      }
    },
  };
}

class OverviewPage implements m.ClassComponent<RecMgrAttrs> {
  private objToId = new ObjToId();

  view({attrs}: m.CVnode<RecMgrAttrs>) {
    const recMgr = attrs.recMgr;

    return [
      m('header', 'Target platform'),
      m(SegmentedButtons, {
        className: 'platform-selector',
        options: TARGET_PLATFORMS.map((p) => ({label: p.name, icon: p.icon})),
        selectedOption: TARGET_PLATFORMS.findIndex(
          (p) => p.id === recMgr.currentPlatform,
        ),
        onOptionSelected: (num) => {
          const platformId = TARGET_PLATFORMS[num].id;
          recMgr.setPlatform(platformId);
          recMgr.loadDefaultConfig();
        },
      }),

      m(RecordConfigSelector, {recMgr}),

      m(TransportSelector, {recMgr}),

      recMgr.currentProvider && [
        m(TargetSelector, {
          recMgr,
          provider: recMgr.currentProvider,
          key: `${this.objToId.getKey(recMgr.currentProvider)}-${recMgr.currentPlatform}`,
        }),
      ],

      recMgr.currentTarget && [
        m(TargetDetails, {
          recMgr,
          target: recMgr.currentTarget,
          key: `${this.objToId.getKey(recMgr.currentTarget)}-${recMgr.currentPlatform}`,
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
    ];
  }
}

class RecordConfigSelector implements m.ClassComponent<RecMgrAttrs> {
  view({attrs}: m.CVnode<RecMgrAttrs>) {
    const recMgr = attrs.recMgr;
    const presets = getPresetsForPlatform(recMgr.currentPlatform);
    const isEmptySelected =
      recMgr.selectedConfigId === undefined &&
      recMgr.isConfigModified === false &&
      !recMgr.hasCustomTraceConfig &&
      !recMgr.recordConfig.hasActiveProbes();

    return [
      m('header', 'Trace config'),
      m('.pf-config-selector', [
        m('h3', 'Quick starts'),
        m('.pf-config-selector__grid', [
          ...presets.map((p) =>
            this.renderCard(
              p.icon,
              p.title,
              p.subtitle,
              recMgr.selectedConfigId === `preset:${p.id}` &&
                recMgr.isConfigModified === false,
              () =>
                recMgr.loadConfig({
                  config: p.session,
                  configId: `preset:${p.id}`,
                  configName: p.title,
                }),
            ),
          ),
          this.renderCard(
            'clear_all',
            'Empty',
            'Start fresh',
            isEmptySelected,
            () => {
              recMgr.clearSession();
              recMgr.clearSelectedConfig();
            },
          ),
        ]),
        this.renderSavedConfigsSection(recMgr),
      ]),
    ];
  }

  private renderSavedConfigsSection(recMgr: RecordingManager) {
    const hasActiveProbes = recMgr.recordConfig.hasActiveProbes();
    const hasUnsavedCustomConfig =
      recMgr.hasCustomTraceConfig && recMgr.selectedConfigId === undefined;
    const shouldHighlightSave =
      hasUnsavedCustomConfig ||
      (hasActiveProbes && recMgr.selectedConfigId === undefined) ||
      recMgr.isConfigModified === true;
    return [
      m('h3', 'User configs'),
      m('.pf-config-selector__grid', [
        // Saved configs
        ...recMgr.savedConfigs.map((saved) => {
          const isSelected =
            recMgr.selectedConfigId === `saved:${saved.name}` &&
            recMgr.isConfigModified === false;
          const config = saved.config;
          const isCustom = config.kind === 'custom';
          return m(
            Card,
            {
              className:
                'pf-preset-card' +
                (isSelected ? ' pf-preset-card--selected' : ''),
              onclick: () => this.loadSavedConfig(recMgr, saved),
              tabindex: 0,
            },
            m(Icon, {icon: isCustom ? 'description' : 'bookmark'}),
            m('.pf-preset-card__title', saved.name),
            isCustom &&
              m(
                '.pf-preset-card__subtitle',
                `Imported from ${config.customConfigFileName ?? 'textproto'}`,
              ),
            m('.pf-preset-card__actions', [
              !isCustom &&
                m(Button, {
                  icon: 'save',
                  compact: true,
                  title: 'Overwrite with current settings',
                  onclick: (e: Event) => {
                    e.stopPropagation();
                    if (
                      confirm(
                        `Overwrite config "${saved.name}" with current settings?`,
                      )
                    ) {
                      recMgr.saveConfig(saved.name);
                      recMgr.app.raf.scheduleFullRedraw();
                    }
                  },
                }),
              m(Button, {
                icon: 'share',
                compact: true,
                title: 'Share configuration',
                onclick: (e: Event) => {
                  e.stopPropagation();
                  shareRecordConfig(saved.config);
                },
              }),
              m(Button, {
                icon: Icons.Delete,
                compact: true,
                title: 'Delete configuration',
                onclick: (e: Event) => {
                  e.stopPropagation();
                  if (confirm(`Delete "${saved.name}"?`)) {
                    recMgr.deleteConfig(saved.name);
                    recMgr.app.raf.scheduleFullRedraw();
                  }
                },
              }),
            ]),
          );
        }),
        // Save card - only show when highlighted (custom config)
        shouldHighlightSave &&
          m(
            Card,
            {
              className:
                'pf-preset-card pf-preset-card--dashed pf-preset-card--highlight',
              onclick: () => {
                const name = prompt('Enter a name for this configuration:');
                if (name?.trim()) {
                  const trimmedName = name.trim();
                  if (recMgr.savedConfigs.some((s) => s.name === trimmedName)) {
                    alert(
                      `A configuration named "${trimmedName}" already exists.`,
                    );
                    return;
                  }
                  const saved = recMgr.saveConfig(trimmedName);
                  this.loadSavedConfig(recMgr, saved);
                  recMgr.app.raf.scheduleFullRedraw();
                }
              },
              tabindex: 0,
            },
            m(Icon, {icon: hasUnsavedCustomConfig ? 'description' : 'tune'}),
            m(
              '.pf-preset-card__title',
              hasUnsavedCustomConfig
                ? recMgr.customConfigFileName ?? 'Imported config'
                : 'Custom',
            ),
            m('.pf-preset-card__subtitle', 'Click to save'),
          ),
        this.renderImportCard(recMgr),
      ]),
    ];
  }

  private loadSavedConfig(recMgr: RecordingManager, saved: SavedSessionSchema) {
    recMgr.loadConfig({
      config: saved.config,
      configId: `saved:${saved.name}`,
      configName: saved.name,
    });
  }

  private renderImportCard(recMgr: RecordingManager) {
    return m(
      Card,
      {
        className: 'pf-preset-card pf-preset-card--dashed',
        onclick: () => this.openImportDialog(recMgr),
        tabindex: 0,
      },
      m(Icon, {icon: 'upload_file'}),
      m('.pf-preset-card__title', 'Import'),
      m('.pf-preset-card__subtitle', 'Load textproto'),
    );
  }

  private openImportDialog(recMgr: RecordingManager) {
    const input = document.createElement('input');
    input.type = 'file';
    input.onchange = async () => {
      const file = input.files?.[0];
      if (!file) return;
      const text = await file.text();
      const res = await traceConfigToPb(text);
      if (!res.ok) {
        showModal({
          title: 'Import error',
          content: `Failed to parse config: ${res.error}`,
        });
        return;
      }
      const config = protos.TraceConfig.decode(res.value);
      recMgr.setCustomTraceConfig(config, file.name);
    };
    input.click();
  }

  private renderCard(
    icon: string,
    title: string,
    subtitle: string,
    isSelected: boolean,
    onclick: () => void,
    extraClass = '',
  ) {
    return m(
      Card,
      {
        className: `pf-preset-card${extraClass}${isSelected ? ' pf-preset-card--selected' : ''}`,
        onclick,
        tabindex: 0,
      },
      m(Icon, {icon}),
      m('.pf-preset-card__title', title),
      m('.pf-preset-card__subtitle', subtitle),
    );
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
              m.redraw();
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
    const isValid = attrs.recMgr.recordConfig.traceConfig.mode !== 'LONG_TRACE';
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
          disabled: isRecording || !isValid,
          onclick: () => attrs.recMgr.startTracing().then(() => m.redraw()),
        }),
        m(Button, {
          label: 'Stop',
          icon: 'stop',
          className: 'stop',
          iconFilled: true,
          disabled: !isRecording || !isValid,
          onclick: () => session?.session?.stop().then(() => m.redraw()),
        }),
        m(Button, {
          label: 'Cancel',
          icon: 'cancel',
          className: 'cancel',
          iconFilled: true,
          disabled: !isRecording || !isValid,
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
              onclick: () =>
                download({
                  fileName: this.session.fileName,
                  content: traceData,
                }),
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
