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

import './styles.scss';
import m from 'mithril';
import {v4} from 'uuid';
import {assertTrue} from '../../base/assert';
import type {App} from '../../public/app';
import {Button, type ButtonAttrs} from '../../widgets/button';
import {Checkbox} from '../../widgets/checkbox';
import {Intent} from '../../widgets/common';
import {
  type MultiSelectDiff,
  type MultiSelectOption,
  PopupMultiSelect,
} from '../../widgets/multiselect';
import {Popup, type PopupAttrs, PopupPosition} from '../../widgets/popup';
import {Select} from '../../widgets/select';
import {Tabs} from '../../widgets/tabs';
import {TextInput} from '../../widgets/text_input';
import {Tree, TreeNode} from '../../widgets/tree';
import {Slider} from '../dev.perfetto.RecordTraceV2/pages/widgets/slider';
import {Toggle} from '../dev.perfetto.RecordTraceV2/pages/widgets/toggle';
import {
  type ArmTelemetryCpuSpec,
  type ArmTelemetryPmuMetric,
  getCpuId,
} from './arm_telemetry_spec';
import {parseArmTelemetrySpec} from './arm_telemetry_spec_manager_impl';
import type {
  ArmTelemetrySpecChange,
  ArmTelemetrySpecManager,
} from './arm_telemetry_spec_manager';
import type {
  ProbeSetting,
  RecordProbe,
  RecordSubpage,
} from '../dev.perfetto.RecordTraceV2/config/config_interfaces';
import type {TraceConfigBuilder} from '../dev.perfetto.RecordTraceV2/config/trace_config_builder';
import type {RecordingManager} from '../dev.perfetto.RecordTraceV2/recording_manager';

const DEFAULT_SAMPLE_FREQUENCY = 100;
const DEFAULT_SAMPLE_PERIOD = 10000;

export function pmuRecordSection(
  recMgr: RecordingManager,
  app: App,
  specManager: ArmTelemetrySpecManager,
): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'pmu',
    title: 'PMU',
    subtitle: 'Hardware Telemetry',
    icon: 'monitoring',
    probes: [tracedPerf(recMgr, app, specManager)],
  };
}

type SettingRowAttrs = {
  readonly label: string;
  readonly description: string;
  readonly component: m.ClassComponent;
};

class SettingRow implements m.ClassComponent<SettingRowAttrs> {
  view({attrs, children}: m.CVnode<SettingRowAttrs>) {
    return m(
      '.pf-pmu-selector',
      m('header', attrs.label),
      m('header.pf-pmu-selector__description', attrs.description),
      children,
    );
  }
}

type CpuId = string;

export interface PerfCpuConfigurationState {
  cpuSelected: string | undefined;
  pmusSelected: string[];
  onCpuSelected: undefined | ((cpuid: CpuId) => void);
  cpuLeader: string | undefined;
  cpuLeaderUserSelected: boolean;
  targetCpus: string | undefined;
  cpuPerfType: string | undefined;
  sampleByFrequency: boolean;
  period: number;
  frequency: number;
  captureCallstack: boolean;
}

export interface PerfConfigurationState {
  currentTabKey: undefined | string;
  tabs: {
    key: string;
    configuration: PerfCpuConfigurationState;
  }[];
}

export interface SerializedPerfCpuConfigurationState {
  cpuSelected?: string;
  pmusSelected: string[];
  cpuLeader?: string;
  cpuLeaderUserSelected: boolean;
  targetCpus?: string;
  cpuPerfType?: string;
  sampleByFrequency: boolean;
  period: number;
  frequency: number;
  captureCallstack: boolean;
}

export interface SerializedPerfConfigurationState {
  currentTabKey?: string;
  tabs: {
    key: string;
    configuration: SerializedPerfCpuConfigurationState;
  }[];
}

type MethodologyNode = {
  name: string;
  children?: MethodologyNode[];
  isMetric: boolean;
};

function createDefaultPerfCpuConfigurationState(): PerfCpuConfigurationState {
  return {
    cpuSelected: undefined,
    pmusSelected: [],
    onCpuSelected: undefined,
    targetCpus: undefined,
    cpuPerfType: undefined,
    cpuLeader: undefined,
    cpuLeaderUserSelected: false,
    sampleByFrequency: true,
    period: DEFAULT_SAMPLE_PERIOD,
    frequency: DEFAULT_SAMPLE_FREQUENCY,
    captureCallstack: false,
  };
}

function buildCpuList(
  specManager: ArmTelemetrySpecManager,
): Map<CpuId, string> {
  const res = new Map<CpuId, string>();
  specManager.registeredCpuids().forEach((cpuid) => {
    const desc = specManager.getCpuDesc(cpuid);
    res.set(cpuid, desc.product_configuration.product_name);
  });
  return res;
}

function getRegisteredCpuDesc(
  specManager: ArmTelemetrySpecManager,
  cpuid: string,
): ArmTelemetryCpuSpec | undefined {
  if (!specManager.registeredCpuids().includes(cpuid)) {
    return undefined;
  }
  return specManager.getCpuDesc(cpuid);
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

function stringValue(value: unknown): string | undefined {
  if (typeof value !== 'string') {
    return undefined;
  }
  const trimmed = value.trim();
  return trimmed === '' ? undefined : trimmed;
}

function uniqueStrings(value: unknown): string[] {
  if (!Array.isArray(value)) {
    return [];
  }
  return Array.from(
    new Set(value.filter((item): item is string => typeof item === 'string')),
  );
}

function positiveIntegerOrDefault(
  value: unknown,
  defaultValue: number,
): number {
  if (typeof value === 'number' && Number.isSafeInteger(value) && value > 0) {
    return value;
  }
  return defaultValue;
}

function positiveIntegerString(value: unknown): string | undefined {
  const stringified =
    typeof value === 'number' ? String(value) : stringValue(value);
  if (stringified === undefined) {
    return undefined;
  }

  const numericValue = Number(stringified);
  if (Number.isSafeInteger(numericValue) && numericValue > 0) {
    return stringified;
  }
  return undefined;
}

function targetCpusValue(value: unknown): string | undefined {
  const cpus = stringValue(value);
  if (cpus === undefined) {
    return undefined;
  }

  const [, error] = parseTargetCpus(cpus);
  return error === undefined ? cpus : undefined;
}

export function normalizeCpuLeader(
  cpuLeader: unknown,
  pmusSelected: string[],
  keepCurrentLeader: boolean,
): string | undefined {
  const leader = stringValue(cpuLeader);
  if (
    keepCurrentLeader &&
    leader !== undefined &&
    pmusSelected.includes(leader)
  ) {
    return leader;
  }
  if (pmusSelected.includes('CPU_CYCLES')) {
    return 'CPU_CYCLES';
  }
  return pmusSelected[0];
}

function deserializePerfCpuConfigurationState(
  specManager: ArmTelemetrySpecManager,
  value: unknown,
): PerfCpuConfigurationState {
  const config = createDefaultPerfCpuConfigurationState();
  if (!isObject(value)) {
    return config;
  }

  const cpuSelected = stringValue(value.cpuSelected);
  const cpuDesc =
    cpuSelected === undefined
      ? undefined
      : getRegisteredCpuDesc(specManager, cpuSelected);
  if (cpuDesc !== undefined) {
    const availablePmus = new Set(Object.keys(cpuDesc.events));
    config.cpuSelected = cpuSelected;
    config.pmusSelected = uniqueStrings(value.pmusSelected).filter((pmu) =>
      availablePmus.has(pmu),
    );

    const savedLeader = stringValue(value.cpuLeader);
    config.cpuLeader = normalizeCpuLeader(
      savedLeader,
      config.pmusSelected,
      true,
    );
    config.cpuLeaderUserSelected =
      typeof value.cpuLeaderUserSelected === 'boolean'
        ? value.cpuLeaderUserSelected
        : false;
  }
  config.targetCpus = targetCpusValue(value.targetCpus);
  config.cpuPerfType = positiveIntegerString(value.cpuPerfType);
  config.sampleByFrequency =
    typeof value.sampleByFrequency === 'boolean'
      ? value.sampleByFrequency
      : config.sampleByFrequency;
  config.period = positiveIntegerOrDefault(value.period, config.period);
  config.frequency = positiveIntegerOrDefault(
    value.frequency,
    config.frequency,
  );
  config.captureCallstack =
    typeof value.captureCallstack === 'boolean'
      ? value.captureCallstack
      : config.captureCallstack;

  return config;
}

export function serializePerfConfigurationState(
  state: PerfConfigurationState,
): SerializedPerfConfigurationState {
  return {
    currentTabKey: state.currentTabKey,
    tabs: state.tabs.map((tab) => ({
      key: tab.key,
      configuration: {
        cpuSelected: tab.configuration.cpuSelected,
        pmusSelected: [...tab.configuration.pmusSelected],
        cpuLeader: tab.configuration.cpuLeader,
        cpuLeaderUserSelected: tab.configuration.cpuLeaderUserSelected,
        targetCpus: tab.configuration.targetCpus,
        cpuPerfType: tab.configuration.cpuPerfType,
        sampleByFrequency: tab.configuration.sampleByFrequency,
        period: tab.configuration.period,
        frequency: tab.configuration.frequency,
        captureCallstack: tab.configuration.captureCallstack,
      },
    })),
  };
}

export function deserializePerfConfigurationState(
  specManager: ArmTelemetrySpecManager,
  state: PerfConfigurationState,
  value: unknown,
): void {
  state.currentTabKey = undefined;
  state.tabs = [];

  if (!isObject(value) || !Array.isArray(value.tabs)) {
    return;
  }

  const usedKeys = new Set<string>();
  state.tabs = value.tabs.map((tab) => {
    const serializedTab = isObject(tab) ? tab : {};
    const serializedKey = stringValue(serializedTab.key);
    const key =
      serializedKey !== undefined && !usedKeys.has(serializedKey)
        ? serializedKey
        : `tab-perf-${v4()}`;
    usedKeys.add(key);
    return {
      key,
      configuration: deserializePerfCpuConfigurationState(
        specManager,
        serializedTab.configuration,
      ),
    };
  });

  const currentTabKey = stringValue(value.currentTabKey);
  state.currentTabKey =
    currentTabKey !== undefined && usedKeys.has(currentTabKey)
      ? currentTabKey
      : state.tabs[0]?.key;
}

function refreshPmuTabsForCpuSpec(
  state: PerfConfigurationState,
  change: ArmTelemetrySpecChange,
): void {
  if (change.kind === 'ADD') {
    return;
  }

  if (change.kind === 'CLEAR') {
    state.tabs.forEach((tab) => {
      tab.configuration.cpuSelected = undefined;
      tab.configuration.pmusSelected = [];
      tab.configuration.cpuLeader = undefined;
      tab.configuration.cpuLeaderUserSelected = false;
    });
    return;
  }

  const cpuId = getCpuId(change.desc);
  state.tabs.forEach((tab) => {
    if (
      tab.configuration.cpuSelected === cpuId &&
      tab.configuration.onCpuSelected
    ) {
      tab.configuration.onCpuSelected(cpuId);
    }
  });
}

class PerfCpuConfigurationController {
  state: PerfCpuConfigurationState | undefined = undefined;
  pmuOptions: MultiSelectOption[] = [];
  metricOptions: MultiSelectOption[] = [];
  topDownTree: MethodologyNode | undefined = undefined;
  availableMetrics: {[keyof: string]: ArmTelemetryPmuMetric} = {};
  app?: App;
  specManager?: ArmTelemetrySpecManager;

  initialize(
    state: PerfCpuConfigurationState,
    app: App,
    specManager: ArmTelemetrySpecManager,
  ) {
    this.state = state;
    this.app = app;
    this.specManager = specManager;
    this.state.onCpuSelected = this.onCpuSelectionChange.bind(this);

    this.initOptions();
  }

  get cpus(): Map<CpuId, string> {
    return buildCpuList(this.specManager!);
  }

  onCpuSelectionChange(cpuid: CpuId) {
    this.state!.cpuSelected = cpuid;

    const cpuEntry = this.cpus.get(cpuid);
    if (cpuEntry === undefined) {
      return;
    }
    this.state!.pmusSelected = [];
    this.state!.cpuLeader = undefined;
    this.state!.cpuLeaderUserSelected = false;
    this.initOptions();
  }

  initOptions() {
    const cpu = this.cpuDesc;
    if (!cpu) {
      this.pmuOptions = [];
      this.metricOptions = [];
      this.topDownTree = undefined;
      this.app?.raf.scheduleFullRedraw();
      return;
    }

    this.pmuOptions = Object.entries(cpu.events).map(([key, _value]) => {
      return {
        id: key,
        name: key,
        checked: this.state!.pmusSelected.includes(key),
      };
    });

    this.metricOptions = Object.entries(cpu.metrics).map(([key, value]) => {
      return {
        id: key,
        name: value.title,
        checked: value.events.every((eventName) =>
          this.state!.pmusSelected.includes(eventName),
        ),
      };
    });

    const decisionTree = cpu.methodologies.topdown_methodology.decision_tree;

    const createNode = (name: string): MethodologyNode => {
      const metricNode = decisionTree.metrics.find(
        (metric) => metric.name === name,
      );
      if (metricNode) {
        return {
          name,
          isMetric: Object.keys(cpu.metrics).includes(name),
          children: metricNode.next_items.map((childName) =>
            createNode(childName),
          ),
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
      name: 'Top-down Methodology',
      children: decisionTree.root_nodes.map((metric) => createNode(metric)),
      isMetric: false,
    };

    this.app?.raf.scheduleFullRedraw();
  }

  get cpuDesc(): ArmTelemetryCpuSpec | undefined {
    const state = this.state;
    if (state?.cpuSelected === undefined) {
      return undefined;
    }
    return this.specManager!.getCpuDesc(state.cpuSelected);
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
      this.pmuOptions
        .filter((option) => option.checked)
        .map((option) => option.id),
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
    const previousLeader = this.state!.cpuLeader;
    this.state!.cpuLeader = normalizeCpuLeader(
      previousLeader,
      this.state!.pmusSelected,
      this.state!.cpuLeaderUserSelected,
    );
    this.state!.cpuLeaderUserSelected =
      this.state!.cpuLeaderUserSelected &&
      previousLeader !== undefined &&
      this.state!.cpuLeader === previousLeader;
  }
}

interface PerfCpuConfigurationAttrs {
  readonly recMgr: RecordingManager;
  readonly state: PerfCpuConfigurationState;
  readonly app: App;
  readonly specManager: ArmTelemetrySpecManager;
}

class PerfCpuConfiguration implements m.ClassComponent<PerfCpuConfigurationAttrs> {
  constructor() {
    this.controller = new PerfCpuConfigurationController();
  }

  private controller: PerfCpuConfigurationController;

  oninit({attrs}: m.Vnode<PerfCpuConfigurationAttrs, this>) {
    this.controller.initialize(attrs.state, attrs.app, attrs.specManager);
  }

  view() {
    const options = [...this.controller.cpus.entries()];

    return m(
      '.pf-pmu-tab-content',
      m('header', m('h1', 'CPU selection')),
      m(
        SettingRow,
        {
          label: 'CPU',
          description: 'Select the CPU to record',
        } as SettingRowAttrs,
        m(
          Select,
          {
            onchange: (e: Event) => {
              const el = e.target as HTMLSelectElement;
              this.controller.onCpuSelectionChange(el.value);
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
        SettingRow,
        {
          label: 'Target CPUs',
          description: 'Enter a comma-separated list of integers or ranges.',
        } as SettingRowAttrs,
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
        SettingRow,
        {
          label: 'Perf CPU Event type',
          description:
            'The event type perf_event associate with this CPU model. Retrieve it in /sys/bus/event_source/devices/<cpu-cluster>/type',
        } as SettingRowAttrs,
        m(TextInput, {
          id: 'Perf CPU Event type',
          type: 'string',
          placeholder: '8',
          autofocus: false,
          value: this.controller.state!.cpuPerfType ?? '',
          oninput: (e: Event) => {
            this.controller.state!.cpuPerfType = (
              e.target as HTMLInputElement
            ).value.trim();
          },
        }),
      ),
      m('header', m('h1', 'PMU selection')),
      m(
        SettingRow,
        {
          label: 'Pmu',
          description: 'Directly select the PMU you want to record.',
        } as SettingRowAttrs,
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
        SettingRow,
        {
          label: 'Metrics',
          description: 'Select the Metric you want to record.',
        } as SettingRowAttrs,
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
            SettingRow,
            {
              label: 'Top-down methodology',
              description:
                'Review the metrics defined by the top down methodology you want to enable.',
            } as SettingRowAttrs,
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
        SettingRow,
        {
          label: 'Pmu leader',
          description: 'Select the PMU which will drive the capture.',
        } as SettingRowAttrs,
        m(
          Select,
          {
            oninput: (e: Event) => {
              if (!e.target) return;
              this.controller.state!.cpuLeader = (
                e.target as HTMLSelectElement
              ).value;
              this.controller.state!.cpuLeaderUserSelected = true;
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
      new Toggle({
        title: 'Capture callstack',
        descr: `If enabled, the callstack is captured alongside the PMU value. At higher sampling frequencies, this may impact performance.`,
        default: this.controller.state!.captureCallstack,
        onChange: (enabled: boolean) => {
          this.controller.state!.captureCallstack = enabled;
        },
      }).render(),
      new Toggle({
        title: 'Sample by frequency',
        descr: `If this is enabled, the kernel attempts
          to sample the leader at the frequency requested.`,
        default: this.controller.state!.sampleByFrequency,
        onChange: (enabled: boolean) => {
          this.controller.state!.sampleByFrequency = enabled;
        },
      }).render(),
      new Slider({
        title: 'Period',
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
        default: this.controller.state!.period,
        onChange: (val: number) => {
          this.controller.state!.period = val;
        },
      }).render(),
      new Slider({
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
        default: this.controller.state!.frequency,
        onChange: (val: number) => {
          this.controller.state!.frequency = val;
        },
      }).render(),
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
  readonly recMgr: RecordingManager;
  readonly state: PerfConfigurationState;
  readonly app: App;
  readonly specManager: ArmTelemetrySpecManager;
}

class PerfConfiguration implements m.ClassComponent<PerfConfigurationAttrs> {
  private specChangeCallback?: Disposable;

  oninit({
    attrs: {state, app, specManager},
  }: m.Vnode<PerfConfigurationAttrs, this>) {
    this.specChangeCallback = specManager.addOnChangeCallback((change) => {
      refreshPmuTabsForCpuSpec(state, change);
      app.raf.scheduleFullRedraw();
    });
  }

  onremove() {
    this.specChangeCallback?.[Symbol.dispose]();
    this.specChangeCallback = undefined;
  }

  view({
    attrs: {recMgr, state, app, specManager},
  }: m.Vnode<PerfConfigurationAttrs, this>) {
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

            const desc = parseArmTelemetrySpec(reader.result);
            if (desc === undefined) {
              throw new Error('Could not parse CPU description file.');
            }
            specManager.update(desc);
            app.raf.scheduleFullRedraw();
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
      (() => {
        const addTabButton = m(Button, {
          icon: 'add',
          className: 'pf-tabs__new-tab-btn',
          onclick: () => {
            const key = `tab-perf-${v4()}`;
            state.tabs.push({
              key,
              configuration: createDefaultPerfCpuConfigurationState(),
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
            specManager,
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

function tracedPerf(
  recMgr: RecordingManager,
  app: App,
  specManager: ArmTelemetrySpecManager,
): RecordProbe {
  const state: PerfConfigurationState = {
    currentTabKey: undefined,
    tabs: [],
  };

  const settings = {
    testSetting: {
      render() {
        return m(
          '.pf-pmu-record-setting',
          m(PerfConfiguration, {
            recMgr,
            state,
            app,
            specManager,
          }),
        );
      },
      serialize() {
        return serializePerfConfigurationState(state);
      },
      deserialize(serializedState: unknown) {
        deserializePerfConfigurationState(specManager, state, serializedState);
      },
    } as ProbeSetting,
  };
  return {
    id: 'pmu',
    title: 'PMU configuration',
    description: 'Record hardware counters and apply top-down methodology',
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

        const cpuDesc = specManager.getCpuDesc(cpuState.cpuSelected);
        if (cpuDesc === undefined) {
          return;
        }

        if (cpuState.cpuPerfType === undefined) {
          alert('PMU Configuration Error: Perf CPU event type is required');
          return;
        }

        const cpuPerfType = Number(cpuState.cpuPerfType);
        if (!Number.isSafeInteger(cpuPerfType) || cpuPerfType <= 0) {
          alert('PMU Configuration Error: Invalid perf CPU event type');
          return;
        }

        if (
          cpuState.sampleByFrequency &&
          (!Number.isSafeInteger(cpuState.frequency) || cpuState.frequency <= 0)
        ) {
          alert('PMU Configuration Error: Invalid sampling frequency');
          return;
        }

        if (
          !cpuState.sampleByFrequency &&
          (!Number.isSafeInteger(cpuState.period) || cpuState.period <= 0)
        ) {
          alert('PMU Configuration Error: Invalid sampling period');
          return;
        }

        let targetCpus: number[] | undefined;
        if (cpuState.targetCpus) {
          const [cpus, error] = parseTargetCpus(cpuState.targetCpus);
          if (error !== undefined) {
            alert(`PMU Configuration Error: ${error}`);
            return;
          }
          targetCpus = cpus;
        }

        tc.addDataSource('linux.system_info');
        const config = tc.addDataSource(`linux.perf ${index}`);
        config.name = 'linux.perf';
        config.perfEventConfig = {};
        const perfConf = config.perfEventConfig;

        perfConf.timebase = {
          name: cpuState.cpuLeader,
          rawEvent: {
            type: cpuPerfType,
            config: Number(cpuDesc.events[cpuState.cpuLeader].code),
          },
        };

        perfConf.followers = cpuState.pmusSelected
          .filter((pmu) => pmu !== cpuState.cpuLeader)
          .map((pmu) => {
            return {
              name: pmu,
              rawEvent: {
                type: cpuPerfType,
                config: Number(cpuDesc.events[pmu].code),
              },
            };
          });

        if (targetCpus !== undefined) {
          perfConf.targetCpu = targetCpus;
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

function parseCpuNumber(cpu: string): number | undefined {
  if (!/^\d+$/.test(cpu)) {
    return undefined;
  }
  const value = Number(cpu);
  return Number.isSafeInteger(value) ? value : undefined;
}

function parseTargetCpus(cpus: string | undefined): [number[], string?] {
  const cpuSet = new Set<number>();

  if (cpus === undefined) {
    return [[], 'Undefined cpu input string'];
  }

  const parts = cpus.split(',');
  for (const partRaw of parts) {
    const part = partRaw.trim();

    if (part === '') {
      return [[], 'Invalid target CPU value'];
    }

    if (part.includes('-')) {
      const rangeParts = part.split('-');
      if (rangeParts.length !== 2) {
        return [[], `Invalid CPU range: ${part}`];
      }
      const [startStrRaw, endStrRaw] = rangeParts;
      const startStr = startStrRaw.trim();
      const endStr = endStrRaw.trim();
      const start = parseCpuNumber(startStr);
      const end = parseCpuNumber(endStr);

      if (start === undefined || end === undefined || start > end) {
        return [[], `Invalid CPU range: ${part}`];
      }

      for (let i = start; i <= end; i++) {
        cpuSet.add(i);
      }
    } else {
      const val = parseCpuNumber(part);
      if (val === undefined) {
        return [[], `Invalid target CPU value: ${part}`];
      }
      cpuSet.add(val);
    }
  }

  return [Array.from(cpuSet)];
}
