// Copyright (C) 2025 The Android Open Source Project
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
import {v4} from 'uuid';
import {assertTrue} from '../../base/assert';
import {DisposableStack} from '../../base/disposable_stack';
import {App} from '../../public/app';
import {Button, ButtonAttrs} from '../../widgets/button';
import {Checkbox} from '../../widgets/checkbox';
import {Intent} from '../../widgets/common';
import {
  MultiSelectDiff,
  MultiSelectOption,
  PopupMultiSelect,
} from '../../widgets/multiselect';
import {Popup, PopupAttrs, PopupPosition} from '../../widgets/popup';
import {Select} from '../../widgets/select';
import {Switch} from '../../widgets/switch';
import {Tabs} from '../../widgets/tabs';
import {TextInput} from '../../widgets/text_input';
import {Tree, TreeNode} from '../../widgets/tree';
import {
  ArmTelemetryCpuSpec,
  ArmTelemetryPmuMetric,
  getCpuId,
} from '../com.arm.ArmTelemetrySpec/arm_telemetry_spec';
import ArmTelemetrySpecPlugin from '../com.arm.ArmTelemetrySpec';
import {
  ProbeSetting,
  RecordProbe,
  RecordSubpage,
} from '../dev.perfetto.RecordTraceV2/config/config_interfaces';
import {TraceConfigBuilder} from '../dev.perfetto.RecordTraceV2/config/trace_config_builder';
import {RecordingManager} from '../dev.perfetto.RecordTraceV2/recording_manager';

export function pmuRecordSection(
  recMgr: RecordingManager,
  app: App,
): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'pmu',
    title: 'PMU',
    subtitle: 'Hardware Telemetry',
    icon: 'monitoring',
    probes: [tracedPerf(recMgr, app)],
  };
}

type SelectorAttr = {
  label: string;
  description: string;
  component: m.ClassComponent;
};

class Selector implements m.ClassComponent<SelectorAttr> {
  view({attrs, children}: m.CVnode<SelectorAttr>) {
    return m(
      '.selector',
      m('header', attrs.label),
      m('header.descr', attrs.description),
      children,
    );
  }
}

interface ToggleAttrs {
  title: string;
  descr: string;
  cssClass?: string;
  enabled: boolean;
  onToggle: (enabled: boolean) => void;
}

class Toggle implements m.ClassComponent<ToggleAttrs> {
  view({attrs}: m.CVnode<ToggleAttrs>) {
    return m(
      `.pf-toggle.pmu-toggle${attrs.cssClass ?? ''}`,
      m(Switch, {
        className: 'pmu-toggle__switch',
        checked: attrs.enabled,
        oninput: (e: InputEvent) =>
          attrs.onToggle((e.target as HTMLInputElement).checked),
        label: attrs.title,
      }),
      attrs.descr ? m('div.pmu-toggle__desc', attrs.descr) : null,
    );
  }
}

export interface SliderAttrs {
  title: string;
  icon?: string;
  cssClass?: string;
  isTime?: boolean;
  unit: string;
  values: number[];
  val: number;
  onValueChange: (newVal: number) => void;
  min?: number;
  description?: string;
  disabled?: boolean;
  zeroIsDefault?: boolean;
}

export class Slider implements m.ClassComponent<SliderAttrs> {
  onTimeValueChange(attrs: SliderAttrs, hms: string) {
    try {
      const date = new Date(`1970-01-01T${hms}.000Z`);
      if (isNaN(date.getTime())) return;
      attrs.onValueChange(date.getTime());
    } catch {}
  }

  onSliderChange(attrs: SliderAttrs, newIdx: number) {
    attrs.onValueChange(attrs.values[newIdx]);
  }

  view({attrs}: m.CVnode<SliderAttrs>) {
    const id = attrs.title.replace(/[^a-z0-9]/gim, '_').toLowerCase();
    const maxIdx = attrs.values.length - 1;
    const val = attrs.val;
    let min = attrs.min || 1;
    if (attrs.zeroIsDefault) {
      min = Math.min(0, min);
    }
    const description = attrs.description;
    const disabled = attrs.disabled;

    let idx = 0;
    for (; idx < attrs.values.length && attrs.values[idx] < val; idx++) {}

    let spinnerCfg = {};
    if (attrs.isTime) {
      spinnerCfg = {
        type: 'text',
        pattern: '(0[0-9]|1[0-9]|2[0-3])(:[0-5][0-9]){2}',
        value: new Date(val).toISOString().substr(11, 8),
        oninput: (e: InputEvent) => {
          this.onTimeValueChange(attrs, (e.target as HTMLInputElement).value);
        },
      };
    } else {
      const isDefault = attrs.zeroIsDefault && val === 0;
      spinnerCfg = {
        type: 'number',
        value: isDefault ? '' : val,
        placeholder: isDefault ? '(default)' : '',
        oninput: (e: InputEvent) => {
          attrs.onValueChange(+(e.target as HTMLInputElement).value);
        },
      };
    }
    return m(
      '.slider' + (attrs.cssClass ?? ''),
      m('header', attrs.title),
      description ? m('header.descr', attrs.description) : '',
      attrs.icon !== undefined ? m('i.material-icons', attrs.icon) : [],
      m(`input[id="${id}"][type=range][min=0][max=${maxIdx}][value=${idx}]`, {
        disabled,
        oninput: (e: InputEvent) => {
          this.onSliderChange(attrs, +(e.target as HTMLInputElement).value);
        },
      }),
      m(`input.spinner[min=${min}][for=${id}]`, spinnerCfg),
      m('.unit', attrs.unit),
    );
  }
}

type CpuId = string;

interface PerfCpuConfigurationState {
  cpuSelected: string | undefined;
  pmusSelected: string[];
  onCpuSelected: undefined | ((cpuid: CpuId) => void);
  cpuLeader: string | undefined;
  targetCpus: string | undefined;
  cpuPerfType: number | undefined;
  sampleByFrequency: boolean;
  period: number;
  frequency: number;
  captureCallstack: boolean;
}

interface PerfConfigurationState {
  currentTabKey: undefined | string;
  tabs: {
    key: string;
    configuration: PerfCpuConfigurationState;
  }[];
}

type MethodologyNode = {
  name: string;
  children?: MethodologyNode[];
  isMetric: boolean;
};

function getTelemetryManager() {
  return ArmTelemetrySpecPlugin.getManager();
}

function buildCpuList(): Map<CpuId, string> {
  const registry = getTelemetryManager();
  const res = new Map<CpuId, string>();
  registry.registeredCpuids().forEach((cpuid) => {
    const desc = registry.getCpuDesc(cpuid);
    res.set(cpuid, desc.product_configuration.product_name);
  });
  return res;
}

class PerfCpuConfigurationController {
  state: PerfCpuConfigurationState | undefined = undefined;
  pmuOptions: MultiSelectOption[] = [];
  metricOptions: MultiSelectOption[] = [];
  topDownTree: MethodologyNode | undefined = undefined;
  availableMetrics: {[keyof: string]: ArmTelemetryPmuMetric} = {};
  trash: DisposableStack = new DisposableStack();
  app?: App;

  initialize(state: PerfCpuConfigurationState, app: App) {
    this.state = state;
    this.app = app;
    this.state.onCpuSelected = this.onCpuSelectectionChange.bind(this);

    this.initOptions();
    this.trash.use(
      getTelemetryManager().addOnChangeCallback((_change, desc) => {
        if (
          this.state !== undefined &&
          this.state.cpuSelected === getCpuId(desc)
        ) {
          this.initOptions();
        }
      }),
    );
  }

  get cpus(): Map<CpuId, string> {
    return buildCpuList();
  }

  onCpuSelectectionChange(cpuid: CpuId) {
    const previousCpu = this.state!.cpuSelected;
    this.state!.cpuSelected = cpuid;

    const cpuEntry = this.cpus.get(cpuid);
    if (cpuEntry === undefined) {
      return;
    }

    if (previousCpu !== cpuid) {
      this.state!.pmusSelected = [];
    }
    this.initOptions();
  }

  initOptions() {
    this.pmuOptions = Object.entries(this.cpuDesc?.events || {}).map(
      ([key, _value]) => {
        return {
          id: key,
          name: key,
          checked: this.state!.pmusSelected.includes(key),
        };
      },
    );

    this.metricOptions = Object.entries(this.cpuDesc?.metrics || {}).map(
      ([key, value]) => {
        return {
          id: key,
          name: value.title,
          checked: value.events.every((eventName) =>
            this.state!.pmusSelected.includes(eventName),
          ),
        };
      },
    );

    const cpu = this.cpuDesc;
    if (!cpu) {
      this.topDownTree = undefined;
      this.app?.raf.scheduleFullRedraw();
      return;
    }

    const decisionTree = cpu.methodologies.topdown_methodology.decision_tree;

    const createNode = (name: string): MethodologyNode => {
      const metricNode = decisionTree.metrics.find((metric) => metric.name === name);
      if (metricNode) {
        return {
          name,
          isMetric: Object.keys(cpu.metrics).includes(name),
          children: metricNode.next_items.map((childName) => createNode(childName)),
        };
      }

      const metricGroup = Object.entries(cpu.groups.metrics).find(
        ([key, _value]) => key === name,
      );
      if (metricGroup) {
        return {
          name,
          isMetric: Object.keys(cpu.metrics).includes(name),
          children: metricGroup[1].metrics.map((metric) => createNode(metric)),
        };
      }

      assertTrue(Object.keys(cpu.metrics).includes(name));
      return {
        name,
        isMetric: true,
        children: [],
      };
    };

    this.topDownTree = {
      name: 'TopDown Methodology',
      children: decisionTree.root_nodes.map((metric) => createNode(metric)),
      isMetric: false,
    };

    this.app?.raf.scheduleFullRedraw();
  }

  get cpuDesc(): ArmTelemetryCpuSpec | undefined {
    if (this.state?.cpuSelected === undefined) {
      return undefined;
    }
    return getTelemetryManager().getCpuDesc(this.state.cpuSelected);
  }

  updateOptions(diffs: MultiSelectDiff[], options: MultiSelectOption[]) {
    diffs.forEach((diff) => {
      const option = options.find((opt) => opt.id === diff.id);
      if (option) {
        option.checked = diff.checked;
      }
    });
  }

  processPmuSelection(diffs: MultiSelectDiff[]) {
    const availableMetrics = this.cpuDesc?.metrics;
    if (availableMetrics === undefined) {
      return;
    }

    this.updateOptions(diffs, this.pmuOptions);

    const completePmuList = new Set<string>(
      this.pmuOptions.filter((option) => option.checked).map((option) => option.id),
    );
    this.metricOptions.forEach((metric) => {
      metric.checked = availableMetrics[metric.id].events.every((eventName) =>
        completePmuList.has(eventName),
      );
    });

    this.state!.pmusSelected = this.pmuOptions
      .filter((option) => option.checked)
      .map((option) => option.id);

    this.updateCpuLeader();
  }

  get metricsPmus(): Set<string> {
    const pmus = new Set<string>();
    this.metricOptions.forEach((metric) => {
      if (metric.checked) {
        this.cpuDesc?.metrics[metric.id].events.forEach((eventName) =>
          pmus.add(eventName),
        );
      }
    });
    return pmus;
  }

  processMetricsSelection(diffs: MultiSelectDiff[]) {
    const availableMetrics = this.cpuDesc?.metrics;
    if (availableMetrics === undefined) {
      return;
    }

    const oldMetricsPmus = this.metricsPmus;
    const pmusSelected = this.pmuOptions
      .filter((option) => option.checked)
      .map((option) => option.id);
    const detachedPmus = pmusSelected.filter((pmu) => !oldMetricsPmus.has(pmu));

    this.updateOptions(diffs, this.metricOptions);
    const newMetricsPmus = this.metricsPmus;
    const completePmuList = new Set([...newMetricsPmus, ...detachedPmus]);

    this.metricOptions.forEach((metric) => {
      if (!metric.checked) {
        metric.checked = availableMetrics[metric.id].events.every((eventName) =>
          completePmuList.has(eventName),
        );
      }
    });

    this.pmuOptions.forEach((option) => {
      option.checked = completePmuList.has(option.id);
    });

    this.state!.pmusSelected = this.pmuOptions
      .filter((option) => option.checked)
      .map((option) => option.id);
    this.updateCpuLeader();
  }

  updateCpuLeader() {
    if (
      this.state!.cpuLeader !== undefined &&
      !this.state!.pmusSelected.includes(this.state!.cpuLeader)
    ) {
      this.state!.cpuLeader = undefined;
    }

    if (this.state!.cpuLeader === undefined) {
      if (this.state!.pmusSelected.includes('CPU_CYCLES')) {
        this.state!.cpuLeader = 'CPU_CYCLES';
      } else {
        this.state!.cpuLeader = this.state!.pmusSelected[0];
      }
    }
  }
}

interface PerfCpuConfigurationAttrs {
  recMgr: RecordingManager;
  state: PerfCpuConfigurationState;
  app: App;
}

class PerfCpuConfiguration
  implements m.ClassComponent<PerfCpuConfigurationAttrs>
{
  constructor() {
    this.controller = new PerfCpuConfigurationController();
  }

  private controller: PerfCpuConfigurationController;

  oninit({attrs}: m.Vnode<PerfCpuConfigurationAttrs, this>) {
    this.controller.initialize(attrs.state, attrs.app);
  }

  onremove() {
    this.controller.trash.dispose();
  }

  view() {
    const options = [...this.controller.cpus.entries()];

    return m(
      '.tab-content',
      m('header', m('h1', 'CPU selection')),
      m(
        Selector,
        {
          label: 'CPU',
          description: 'Select the CPU to record',
        } as SelectorAttr,
        m(
          Select,
          {
            onchange: (e: Event) => {
              const el = e.target as HTMLSelectElement;
              this.controller.onCpuSelectectionChange(el.value);
            },
            disabled: options.length === 0,
          },
          m(
            'option',
            {
              value: '',
              disabled: true,
              hidden: true,
              selected: this.controller.state?.cpuSelected === undefined,
            },
            'Select an Option',
          ),
          ...options.map(([cpuid, name]) =>
            m(
              'option',
              {
                value: cpuid,
                selected: cpuid === this.controller.state?.cpuSelected,
              },
              name,
            ),
          ),
        ),
      ),
      m(
        Selector,
        {
          label: 'Target CPUs',
          description: 'Enter a comma-separated list of integers or ranges.',
        } as SelectorAttr,
        m(TextInput, {
          id: 'Target CPUs',
          type: 'string',
          placeholder: 'e.g., 0,2-5,7,9-12',
          autofocus: false,
          value: this.controller.state!.targetCpus ?? '',
          oninput: (e: Event) => {
            this.controller.state!.targetCpus = (
              e.target as HTMLInputElement
            ).value.trim();
          },
        }),
      ),
      m(
        Selector,
        {
          label: 'Perf CPU Event type',
          description:
            'The event type perf_event associate with this CPU model. Retrieve it in /sys/bus/event_source/devices/<cpu-cluster>/type',
        } as SelectorAttr,
        m(TextInput, {
          id: 'Perf CPU Event type',
          type: 'number',
          placeholder: '8',
          autofocus: false,
          value: this.controller.state!.cpuPerfType ?? '8',
          oninput: (e: Event) => {
            this.controller.state!.cpuPerfType = Number(
              (e.target as HTMLInputElement).value,
            );
          },
        }),
      ),
      m('header', m('h1', 'PMU selection')),
      m(
        Selector,
        {
          label: 'Pmu',
          description: 'Directly select the PMU you want to record.',
        } as SelectorAttr,
        m(PopupMultiSelect, {
          position: PopupPosition.Left,
          label: 'PMUs',
          showNumSelected: true,
          repeatCheckedItemsAtTop: true,
          options: this.controller.pmuOptions,
          onChange: (diffs: MultiSelectDiff[]) => {
            this.controller.processPmuSelection(diffs);
          },
        }),
      ),
      m(
        Selector,
        {
          label: 'Metrics',
          description: 'Select the Metric you want to record.',
        } as SelectorAttr,
        m(PopupMultiSelect, {
          position: PopupPosition.Left,
          label: 'Metrics',
          showNumSelected: true,
          repeatCheckedItemsAtTop: true,
          options: this.controller.metricOptions,
          onChange: (diffs: MultiSelectDiff[]) => {
            this.controller.processMetricsSelection(diffs);
          },
        }),
      ),
      this.controller.topDownTree === undefined
        ? ''
        : m(
            Selector,
            {
              label: 'TopDown methodology',
              description:
                'Review the metrics defined by the top down methodology you want to enable.',
            } as SelectorAttr,
            m(
              Popup,
              {
                position: PopupPosition.Left,
                trigger: m(Button, {label: 'Methodology tree', compact: false}),
              } as PopupAttrs,
              m(
                Tree,
                this.controller.topDownTree.children?.map((child) =>
                  this.makeTreeNode(child),
                ),
              ),
            ),
          ),
      m('header', m('h1', 'Sampling configuration')),
      m(
        Selector,
        {
          label: 'Pmu leader',
          description: 'Select the PMU which will drive the capture.',
        } as SelectorAttr,
        m(
          Select,
          {
            oninput: (e: Event) => {
              if (!e.target) return;
              this.controller.state!.cpuLeader = (
                e.target as HTMLSelectElement
              ).value;
            },
          },
          this.controller.state!.pmusSelected.map((pmu) => {
            return m('option', {
              selected: this.controller.state!.cpuLeader === pmu,
              value: pmu,
              label: pmu,
              key: pmu,
            });
          }),
        ),
      ),
      m(Toggle, {
        title: 'Capture callstack',
        cssClass: '.thin',
        descr: `If enabled, the callstack is captured alongside the PMU value. At higher sampling frequencies, this may impact performance.`,
        enabled: this.controller.state!.captureCallstack,
        onToggle: (enabled: boolean) => {
          this.controller.state!.captureCallstack = enabled;
        },
      } as ToggleAttrs),
      m(Toggle, {
        title: 'Sample by frequency',
        cssClass: '.thin',
        descr: `If this is enabled, the kernel attempts
          to sample the leader at the frequency requested.`,
        enabled: this.controller.state!.sampleByFrequency,
        onToggle: (enabled: boolean) => {
          this.controller.state!.sampleByFrequency = enabled;
        },
      } as ToggleAttrs),
      m(Slider, {
        title: 'Counter',
        description:
          'How many occurence of the leader event trigger a sample capture.',
        cssClass: `.thin${
          this.controller.state!.sampleByFrequency === true ? '.greyed-out' : ''
        }`,
        values: [
          1, 2, 5, 10, 25, 50, 100, 500, 1000, 2000, 5000, 10000, 15000, 20000,
          30000, 40000, 50000,
        ],
        unit: 'event',
        min: 0,
        disabled: this.controller.state!.sampleByFrequency === true,
        val: this.controller.state!.period,
        onValueChange: (val: number) => {
          this.controller.state!.period = val;
        },
      } as SliderAttrs),
      m(Slider, {
        title: 'Frequency',
        description: 'Frequency at which the leader should be sampled',
        cssClass: `.thin${
          this.controller.state!.sampleByFrequency === false
            ? '.greyed-out'
            : ''
        }`,
        values: [
          100, 500, 1000, 2000, 5000, 10000, 15000, 20000, 30000, 40000, 50000,
        ],
        unit: 'Hz',
        min: 0,
        disabled: this.controller.state!.sampleByFrequency === false,
        val: this.controller.state!.frequency,
        onValueChange: (val: number) => {
          this.controller.state!.frequency = val;
        },
      } as SliderAttrs),
    );
  }

  makeTreeNode(node: MethodologyNode): m.Children {
    const checked = node.isMetric
      ? this.controller.metricOptions.find((option) => option.id === node.name)
          ?.checked
      : false;
    return m(
      TreeNode,
      {
        left: node.name,
        right: node.isMetric
          ? m(Checkbox, {
              checked,
              onchange: () => {
                this.controller.processMetricsSelection([
                  {id: node.name, checked: !checked},
                ]);
              },
            })
          : undefined,
        startsCollapsed: true,
      },
      node.children?.map((child) => this.makeTreeNode(child)),
    );
  }
}

interface PerfConfigurationAttrs {
  recMgr: RecordingManager;
  state: PerfConfigurationState;
  app: App;
}

class PerfConfiguration implements m.ClassComponent<PerfConfigurationAttrs> {
  view({attrs: {recMgr, state, app}}: m.Vnode<PerfConfigurationAttrs, this>) {
    return m(
      'div',
      m('input.cpu_file[type=file]', {
        style: 'display:none',
        onchange: (e: Event) => {
          if (!(e.target instanceof HTMLInputElement)) {
            throw new Error('Not an input element');
          }
          if (!e.target.files) return;
          const file = e.target.files[0];
          const reader = new FileReader();
          reader.onloadend = () => {
            if (reader.error) {
              throw reader.error;
            }

            if (reader.result === null || typeof reader.result !== 'string') {
              throw new Error(
                'Invalid data present in CPU description file: A JSON object is expected.',
              );
            }

            const desc = getTelemetryManager().parse(reader.result);
            if (desc === undefined) {
              return;
            }
            getTelemetryManager().update(desc);

            state.tabs.forEach((tab) => {
              if (
                tab.configuration.cpuSelected === getCpuId(desc) &&
                tab.configuration.onCpuSelected
              ) {
                tab.configuration.onCpuSelected(getCpuId(desc));
              }
            });
          };
          reader.readAsText(file);
        },
      }),
      m(Button, {
        label: 'Load local CPU file',
        intent: Intent.Primary,
        onclick: (e: Event) => {
          e.preventDefault();
          const fileElement =
            document.querySelector<HTMLInputElement>('input.cpu_file');
          if (!fileElement) return;
          fileElement.click();
        },
      } as ButtonAttrs),
      m('header', m('h1', 'Configurations')),
      (() => {
        const addTabButton = m(Button, {
          icon: 'Add',
          compact: true,
          onclick: () => {
            const key = `tab-perf-${v4()}`;
            const configuration: PerfCpuConfigurationState = {
              cpuSelected: undefined,
              pmusSelected: [],
              onCpuSelected: undefined,
              targetCpus: undefined,
              cpuPerfType: undefined,
              cpuLeader: undefined,
              sampleByFrequency: true,
              period: 10000,
              frequency: 100,
              captureCallstack: false,
            };
            state.tabs.push({
              key,
              configuration,
            });
            state.currentTabKey = key;
          },
        });
        const tabs = state.tabs.map((tab) => ({
          key: tab.key,
          title: `Tab`,
          content: m(PerfCpuConfiguration, {
            key: tab.key,
            recMgr,
            state: tab.configuration,
            app,
          }),
          closeButton: true,
        }));

        return m(Tabs, {
          tabs,
          activeTabKey: state.currentTabKey,
          newTabContent: addTabButton,
          onTabChange: (key: string) => {
            state.currentTabKey = key;
            app.raf.scheduleFullRedraw();
          },
          onTabClose: (key: string) => {
            const closedIndex = state.tabs.findIndex((tab) => tab.key === key);
            state.tabs = state.tabs.filter((tab) => tab.key !== key);
            if (state.currentTabKey === key) {
              const fallbackTab =
                state.tabs[closedIndex] ?? state.tabs[state.tabs.length - 1];
              state.currentTabKey = fallbackTab?.key;
            }
            app.raf.scheduleFullRedraw();
          },
        });
      })(),
    );
  }
}

function tracedPerf(recMgr: RecordingManager, app: App): RecordProbe {
  const state: PerfConfigurationState = {
    currentTabKey: undefined,
    tabs: [],
  };

  const settings = {
    testSetting: {
      render() {
        return m(
          'pmu-record-setting',
          m(PerfConfiguration, {
            recMgr,
            state,
            app,
          }),
        );
      },
      serialize() {
        return {};
      },
      deserialize(_state: unknown) {},
    } as ProbeSetting,
  };
  return {
    id: 'pmu',
    title: 'PMU configuration',
    description: 'Record Hardware counters and apply Topdown Methodology',
    supportedPlatforms: ['ANDROID', 'LINUX'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      state.tabs.forEach((tab, index) => {
        const cpuState = tab.configuration;

        if (
          cpuState.cpuSelected === undefined ||
          cpuState.pmusSelected.length === 0 ||
          cpuState.cpuLeader === undefined
        ) {
          return;
        }

        tc.addDataSource('linux.system_info');

        const config = tc.addDataSource(`linux.perf ${index}`);
        config.name = 'linux.perf';
        config.perfEventConfig = {};
        const perfConf = config.perfEventConfig;

        const cpuDesc = getTelemetryManager().getCpuDesc(
          cpuState.cpuSelected,
        );
        if (cpuDesc === undefined) {
          return;
        }

        perfConf.timebase = {
          name: cpuState.cpuLeader,
          rawEvent: {
            type: cpuState.cpuPerfType ?? 8,
            config: Number(cpuDesc.events[cpuState.cpuLeader].code),
          },
        };

        perfConf.followers = cpuState.pmusSelected
          .filter((pmu) => pmu !== cpuState.cpuLeader)
          .map((pmu) => {
            return {
              name: pmu,
              rawEvent: {
                type: cpuState.cpuPerfType ?? 8,
                config: Number(cpuDesc.events[pmu].code),
              },
            };
          });

        if (cpuState.targetCpus) {
          const [cpus, error] = parseTargetCpus(cpuState.targetCpus);
          if (error === undefined) {
            perfConf.targetCpu = cpus;
          } else {
            alert(`PMU Configuration Error: ${error}`);
            return;
          }
        } else if (cpuState.cpuSelected) {
          perfConf.cpuid = [getCpuId(cpuDesc).replace('0x', '')];
        }

        if (cpuState.sampleByFrequency) {
          perfConf.timebase.frequency = cpuState.frequency;
        } else {
          perfConf.timebase.period = cpuState.period;
        }
        if (cpuState.captureCallstack) {
          perfConf.callstackSampling = {};
        }
      });
    },
  };
}

function parseTargetCpus(cpus: string | undefined): [number[], string?] {
  const cpuSet = new Set<number>();

  if (cpus === undefined) {
    return [[], 'Undefined cpu input string'];
  }

  const parts = cpus.split(',');
  for (const partRaw of parts) {
    const part = partRaw.trim();

    if (part.includes('-')) {
      const [startStr, endStr] = part.split('-');
      const start = Number(startStr);
      const end = Number(endStr);

      if (
        isNaN(start) ||
        isNaN(end) ||
        !Number.isInteger(start) ||
        !Number.isInteger(end) ||
        start > end
      ) {
        return [[], `Invalid CPU range: ${part}`];
      }

      for (let i = start; i <= end; i++) {
        cpuSet.add(i);
      }
    } else {
      const val = Number(part);
      if (isNaN(val) || !Number.isInteger(val)) {
        return [[], `Invalid CPU value: ${part}`];
      }
      cpuSet.add(val);
    }
  }

  return [Array.from(cpuSet)];
}
