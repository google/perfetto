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
import {RecordPluginSchema} from '../serialization_schema';
import {Checkbox} from '../../../widgets/checkbox';
import {Anchor, linkify} from '../../../widgets/anchor';
import {ANDROID_PRESETS, LINUX_PRESETS, Preset} from '../presets';
import {base64Encode} from '../../../base/string_utils';
import {CodeSnippet} from '../../../widgets/code_snippet';
import protos from '../../../protos';
import {traceConfigToTxt} from '../config/trace_config_utils_wasm';
import {Icons} from '../../../base/semantic_icons';
import {shareRecordConfig} from '../config/config_sharing';
import {Card} from '../../../widgets/card';

type RecMgrAttrs = {recMgr: RecordingManager};

function getPresetsForPlatform(platform: string): Preset[] {
  switch (platform) {
    case 'ANDROID':
      return ANDROID_PRESETS;
    case 'LINUX':
      return LINUX_PRESETS;
    case 'CHROME':
    case 'CHROME_OS':
      return [];
    default:
      return [];
  }
}

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

      // After deserializing, if no config is selected, load the first preset
      if (
        recMgr.selectedConfigId === undefined &&
        recMgr.isConfigModified === false
      ) {
        const presets = getPresetsForPlatform(recMgr.currentPlatform);
        if (presets.length > 0) {
          const firstPreset = presets[0];
          recMgr.loadConfig(
            firstPreset.session,
            `preset:${firstPreset.id}`,
            firstPreset.title,
          );
        }
      }
    },
  };
}

class OverviewPage implements m.ClassComponent<RecMgrAttrs> {
  private showCmdline = false;

  view({attrs}: m.CVnode<RecMgrAttrs>) {
    const recMgr = attrs.recMgr;
    // If the current provider is undefined, and we are not in cmdline mode,
    // we might want to auto-select the first provider?
    // But `setPlatform` does that.
    // We use a synthetic "cmdline" transport for the UI.
    const activeTransport = this.showCmdline
      ? 'cmdline'
      : recMgr.currentProvider?.id;

    return [
      m('header', 'Select platform'),
      m(SegmentedButtons, {
        className: 'platform-selector',
        options: TARGET_PLATFORMS.map((p) => ({label: p.name, icon: p.icon})),
        selectedOption: TARGET_PLATFORMS.findIndex(
          (p) => p.id === recMgr.currentPlatform,
        ),
        onOptionSelected: (num) => {
          const platformId = TARGET_PLATFORMS[num].id;
          recMgr.setPlatform(platformId);
          this.showCmdline = false;

          // Auto-load first preset for the new platform
          const presets = getPresetsForPlatform(platformId);
          if (presets.length > 0) {
            const firstPreset = presets[0];
            recMgr.loadConfig(
              firstPreset.session,
              `preset:${firstPreset.id}`,
              firstPreset.title,
            );
          } else {
            // No presets (Chrome/ChromeOS), clear to empty
            recMgr.clearSession();
            recMgr.clearSelectedConfig();
          }
        },
      }),

      m(RecordConfigSelector, {recMgr}),

      m(TransportSelector, {
        recMgr,
        activeTransport,
        onTransportSelect: (id) => {
          if (id === 'cmdline') {
            this.showCmdline = true;
            // We don't unset the provider in the manager, just hide it in UI?
            // Or we should unset it to avoid confusion?
            // recMgr.setProvider(undefined); // This type errors currently
          } else {
            this.showCmdline = false;
            const prov = recMgr.getProvider(id);
            if (prov) recMgr.setProvider(prov);
          }
        },
      }),

      this.showCmdline
        ? m(CmdlineInstructions, {recMgr})
        : recMgr.currentProvider && [
            m(TargetSelector, {
              recMgr,
              provider: recMgr.currentProvider,
              key: new ObjToId().getKey(recMgr.currentProvider),
            }),
          ],

      !this.showCmdline &&
        recMgr.currentTarget && [
          m(TargetDetails, {
            recMgr,
            target: recMgr.currentTarget,
            key: new ObjToId().getKey(recMgr.currentTarget),
          }),
        ],
    ];
  }
}

interface TransportSelectorAttrs {
  recMgr: RecordingManager;
  activeTransport?: string;
  onTransportSelect: (id: string) => void;
}

class TransportSelector implements m.ClassComponent<TransportSelectorAttrs> {
  view({attrs}: m.CVnode<TransportSelectorAttrs>) {
    const options = [];
    const recMgr = attrs.recMgr;

    // Live providers
    for (const provider of recMgr.listProvidersForCurrentPlatform()) {
      const id = provider.id;
      options.push([
        m(`input[type=radio][name=recordingProvider][id=${id}]`, {
          onchange: () => attrs.onTransportSelect(id),
          checked: attrs.activeTransport === id,
        }),
        m(
          `label[for=${id}]`,
          m(Icon, {icon: provider.icon}),
          m('.title', provider.name),
          m('.description', linkify(provider.description)),
        ),
      ]);
    }

    // Cmdline option
    const cmdlineId = 'cmdline';
    options.push([
      m(`input[type=radio][name=recordingProvider][id=${cmdlineId}]`, {
        onchange: () => attrs.onTransportSelect(cmdlineId),
        checked: attrs.activeTransport === cmdlineId,
      }),
      m(
        `label[for=${cmdlineId}]`,
        m(Icon, {icon: 'terminal'}),
        m('.title', 'Command line'),
        m('.description', 'Generate a command to run on the device'),
      ),
    ]);

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

    return [
      m('header', 'Trace config'),
      m('.pf-config-selector', [
        // Quick starts
        m('h3', 'Quick starts'),
        m('.pf-config-selector__grid', [
          ...presets.map((p) => {
            const isSelected =
              recMgr.selectedConfigId === `preset:${p.id}` &&
              recMgr.isConfigModified === false;
            return m(
              Card,
              {
                className:
                  'pf-preset-card' +
                  (isSelected ? ' pf-preset-card--selected' : ''),
                onclick: () =>
                  recMgr.loadConfig(p.session, `preset:${p.id}`, p.title),
                tabindex: 0,
              },
              m(Icon, {icon: p.icon}),
              m('.pf-preset-card__title', p.title),
              m('.pf-preset-card__subtitle', p.subtitle),
              isSelected &&
                m(Icon, {
                  icon: 'check_circle',
                  className: 'pf-preset-card__check-icon',
                }),
            );
          }),
          // Empty card
          m(
            Card,
            {
              className:
                'pf-preset-card' +
                (recMgr.selectedConfigId === undefined &&
                recMgr.isConfigModified === false
                  ? ' pf-preset-card--selected'
                  : ''),
              onclick: () => {
                recMgr.clearSession();
                recMgr.clearSelectedConfig();
              },
              tabindex: 0,
            },
            m(Icon, {icon: 'clear_all'}),
            m('.pf-preset-card__title', 'Empty'),
            m('.pf-preset-card__subtitle', 'Start fresh'),
            recMgr.selectedConfigId === undefined &&
              recMgr.isConfigModified === false &&
              m(Icon, {
                icon: 'check_circle',
                className: 'pf-preset-card__check-icon',
              }),
          ),
        ]),

        // Saved configs
        this.renderSavedConfigsSection(recMgr),
      ]),
    ];
  }

  private renderSavedConfigsSection(recMgr: RecordingManager) {
    const hasActiveProbes = recMgr.recordConfig.hasActiveProbes();

    return [
      m('h3', 'Saved'),
      m('.pf-config-selector__grid', [
        // Saved configs
        ...recMgr.savedConfigs.map((config) => {
          const isSelected =
            recMgr.selectedConfigId === `saved:${config.name}` &&
            recMgr.isConfigModified === false;
          return m(
            Card,
            {
              className:
                'pf-preset-card' +
                (isSelected ? ' pf-preset-card--selected' : ''),
              onclick: () =>
                recMgr.loadConfig(
                  config.config,
                  `saved:${config.name}`,
                  config.name,
                ),
              tabindex: 0,
            },
            m(Icon, {icon: 'bookmark'}),
            m('.pf-preset-card__title', config.name),
            m('.pf-preset-card__actions', [
              m(Button, {
                icon: 'save',
                compact: true,
                title: 'Overwrite with current settings',
                onclick: (e: Event) => {
                  e.stopPropagation();
                  if (
                    confirm(
                      `Overwrite config "${config.name}" with current settings?`,
                    )
                  ) {
                    recMgr.saveConfig(config.name, recMgr.serializeSession());
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
                  shareRecordConfig(config.config);
                },
              }),
              m(Button, {
                icon: Icons.Delete,
                compact: true,
                title: 'Delete configuration',
                onclick: (e: Event) => {
                  e.stopPropagation();
                  if (confirm(`Delete "${config.name}"?`)) {
                    recMgr.deleteConfig(config.name);
                    recMgr.app.raf.scheduleFullRedraw();
                  }
                },
              }),
            ]),
            isSelected &&
              m(Icon, {
                icon: 'check_circle',
                className: 'pf-preset-card__check-icon',
              }),
          );
        }),
        // Save new config
        m(
          Card,
          {
            className: 'pf-preset-card pf-preset-card--dashed',
            onclick: () => {
              if (!hasActiveProbes) return;
              const name = prompt('Enter a name for this configuration:');
              if (name?.trim()) {
                const trimmedName = name.trim();
                if (recMgr.savedConfigs.some((s) => s.name === trimmedName)) {
                  alert(
                    `A configuration named "${trimmedName}" already exists.`,
                  );
                  return;
                }
                const savedConfig = recMgr.serializeSession();
                recMgr.saveConfig(trimmedName, savedConfig);
                recMgr.loadConfig(
                  savedConfig,
                  `saved:${trimmedName}`,
                  trimmedName,
                );
                recMgr.app.raf.scheduleFullRedraw();
              }
            },
            tabindex: 0,
          },
          m(Icon, {icon: 'save'}),
          m(
            '.pf-preset-card__title',
            recMgr.isConfigModified === true
              ? 'Save current *'
              : 'Save current',
          ),
          m(
            '.pf-preset-card__subtitle',
            recMgr.isConfigModified === true
              ? 'Unsaved changes'
              : 'Click to save',
          ),
        ),
      ]),
    ];
  }
}

class CmdlineInstructions implements m.ClassComponent<RecMgrAttrs> {
  private configTxt = '';
  private cmdline = '';
  private inlineCmd = '';
  private docsLink = '';

  constructor({attrs}: m.CVnode<RecMgrAttrs>) {
    const recMgr = attrs.recMgr;

    // Generate the config PBTX (text proto format).
    const cfg = recMgr.genTraceConfig();
    const cfgBytes = protos.TraceConfig.encode(cfg).finish().slice();
    traceConfigToTxt(cfgBytes).then((txt) => {
      this.configTxt = txt;
      m.redraw();
    });

    // Generate platform-specific commands.
    switch (recMgr.currentPlatform) {
      case 'ANDROID':
        this.cmdline =
          'cat config.pbtx | adb shell perfetto' +
          ' -c - --txt -o /data/misc/perfetto-traces/trace.pftrace';
        this.docsLink = 'https://perfetto.dev/docs/quickstart/android-tracing';
        // Also generate inline base64 command for convenience
        const pbBase64 = base64Encode(cfgBytes);
        this.inlineCmd = [
          `echo '${pbBase64}' |`,
          `base64 --decode |`,
          `adb shell "perfetto -c - -o /data/misc/perfetto-traces/trace" &&`,
          `adb pull /data/misc/perfetto-traces/trace /tmp/trace.perfetto-trace`,
        ].join(' \\\n  ');
        break;
      case 'LINUX':
        this.cmdline = 'perfetto -c config.pbtx --txt -o /tmp/trace.pftrace';
        this.docsLink = 'https://perfetto.dev/docs/quickstart/linux-tracing';
        break;
      case 'CHROME':
      case 'CHROME_OS':
        this.docsLink = 'https://perfetto.dev/docs/quickstart/chrome-tracing';
        this.cmdline =
          'There is no cmdline support for Chrome/CrOS.\n' +
          'You must use the recording UI via the extension to record traces.';
        break;
    }
  }

  view({attrs}: m.CVnode<RecMgrAttrs>) {
    const recMgr = attrs.recMgr;

    if (!recMgr.recordConfig.hasActiveProbes()) {
      return m(
        '.record-cmdline',
        m(
          '.note',
          "It looks like you didn't select any data source.",
          'Please select some from the "Probes" menu on the left.',
        ),
      );
    }

    return m('.record-cmdline', [
      // Documentation link
      this.docsLink &&
        m(
          'p',
          'See the documentation on ',
          m(
            Anchor,
            {href: this.docsLink, target: '_blank'},
            this.docsLink.replace('https://', ''),
          ),
        ),

      // Inline command (for Android)
      this.inlineCmd && [
        m('h3', 'Quick command (inline config)'),
        m('p', 'Run this command on your host:'),
        m(CodeSnippet, {
          language: 'Shell',
          text: this.inlineCmd,
        }),
      ],

      // Standard command with config file
      this.cmdline && [
        m('h3', this.inlineCmd ? 'Or use config file' : 'Command'),
        m(CodeSnippet, {
          language: 'Shell',
          text: this.cmdline,
        }),
      ],

      // Config file content
      this.configTxt && [
        m('p', 'Save the file below as: config.pbtx'),
        m(CodeSnippet, {
          language: 'textproto',
          text: this.configTxt,
        }),
      ],
    ]);
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
