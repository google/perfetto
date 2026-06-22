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

import {describe, expect, test, vi} from 'vitest';
import type {App} from '../../public/app';
import type {
  ProbeSetting,
  RecordProbe,
} from '../dev.perfetto.RecordTraceV2/config/config_interfaces';
import {TraceConfigBuilder} from '../dev.perfetto.RecordTraceV2/config/trace_config_builder';
import type {RecordingManager} from '../dev.perfetto.RecordTraceV2/recording_manager';
import {getCpuId, type ArmTelemetryCpuSpec} from './arm_telemetry_spec';
import type {
  ArmTelemetrySpecChange,
  ArmTelemetrySpecChangeCallback,
  ArmTelemetrySpecManager,
} from './arm_telemetry_spec_manager';
import {
  deserializePerfConfigurationState,
  normalizeCpuLeader,
  type PerfConfigurationState,
  type PerfCpuConfigurationState,
  pmuRecordSection,
  serializePerfConfigurationState,
} from './pmu';

type PmuOptionForTest = {
  id: string;
};

type PmuCpuConfigurationComponentForTest = {
  controller: {
    cpus: Map<string, string>;
    pmuOptions: PmuOptionForTest[];
  };
};

type ComponentConstructorForTest<T> = new () => T;

type PerfConfigurationComponentForTest = {
  oninit: (vnode: {
    attrs: {
      state: PerfConfigurationState;
      app: App;
      recMgr: RecordingManager;
    };
  }) => void;
  view: (vnode: {
    attrs: {
      state: PerfConfigurationState;
      app: App;
      recMgr: RecordingManager;
    };
  }) => {
    children: Array<unknown>;
  };
};

type PerfCpuConfigurationComponentConstructorForTest =
  ComponentConstructorForTest<
    PmuCpuConfigurationComponentForTest & {
      oninit: (vnode: {
        attrs: {
          state: PerfCpuConfigurationState;
          app: App;
          recMgr: RecordingManager;
        };
      }) => void;
    }
  >;

const CPU_SPEC: ArmTelemetryCpuSpec = {
  product_configuration: {
    product_name: 'Test CPU',
    part_num: '333',
    major_revision: 1,
    minor_revision: 0,
    implementer: '0x41',
    architecture: 'armv9',
    pmu_architecture: 'pmuv3',
    num_slots: 4,
  },
  events: {
    CPU_CYCLES: {
      code: '0x11',
      title: 'Cycles',
      description: 'Cycles',
    },
    INST_RETIRED: {
      code: '0x08',
      title: 'Instructions',
      description: 'Instructions',
    },
  },
  metrics: {},
  groups: {
    function: {},
    metrics: {},
  },
  methodologies: {
    topdown_methodology: {
      decision_tree: {
        root_nodes: [],
        metrics: [],
      },
    },
  },
};

const CPU_SPEC_1: ArmTelemetryCpuSpec = {
  ...CPU_SPEC,
  product_configuration: {
    ...CPU_SPEC.product_configuration,
    product_name: 'Updated Test CPU',
  },
  events: {
    BR_RETIRED: {
      code: '0x21',
      title: 'Branches retired',
      description: 'Branches retired',
    },
    L1D_CACHE_REFILL: {
      code: '0x03',
      title: 'L1D cache refill',
      description: 'L1D cache refill',
    },
  },
};

const CPU_SPEC_2: ArmTelemetryCpuSpec = {
  ...CPU_SPEC,
  product_configuration: {
    ...CPU_SPEC.product_configuration,
    product_name: 'Added Test CPU',
    part_num: '444',
  },
  events: {
    STALL_BACKEND: {
      code: '0x24',
      title: 'Backend stalls',
      description: 'Backend stalls',
    },
  },
};

class FakeArmTelemetrySpecManager implements ArmTelemetrySpecManager {
  private callbacks = new Set<ArmTelemetrySpecChangeCallback>();
  private cpuDescs = new Map<string, ArmTelemetryCpuSpec | undefined>();

  constructor(
    cpuDesc: ArmTelemetryCpuSpec | undefined,
    private readonly cpuids: string[] = ['0x4114d'],
  ) {
    this.cpuids.forEach((cpuid) => {
      this.cpuDescs.set(cpuid, cpuid === '0x4114d' ? cpuDesc : undefined);
    });
  }

  add(desc: ArmTelemetryCpuSpec): void {
    this.cpuDescs.set(getCpuId(desc), desc);
    this.notify({kind: 'ADD', desc});
  }
  update(desc: ArmTelemetryCpuSpec): void {
    this.cpuDescs.set(getCpuId(desc), desc);
    this.notify({kind: 'UPDATE', desc});
  }
  clear(): void {
    this.cpuDescs.clear();
    this.notify({kind: 'CLEAR'});
  }
  hasSpecs(): boolean {
    return this.cpuDescs.size > 0;
  }
  registeredCpuids(): string[] {
    return [...this.cpuDescs.keys()];
  }
  getCpuDesc(cpuid: string): ArmTelemetryCpuSpec {
    if (!this.cpuDescs.has(cpuid)) {
      throw new Error(`Unexpected CPU ID: ${cpuid}`);
    }
    return this.cpuDescs.get(cpuid) as ArmTelemetryCpuSpec;
  }
  addOnChangeCallback(callback: ArmTelemetrySpecChangeCallback): Disposable {
    this.callbacks.add(callback);
    return {[Symbol.dispose]: () => this.callbacks.delete(callback)};
  }

  notify(change: ArmTelemetrySpecChange): void {
    this.callbacks.forEach((callback) => callback(change));
  }
}

function createFakeAppForTest(): App {
  return {
    raf: {
      scheduleFullRedraw: vi.fn(),
    },
  } as unknown as App;
}

function setTelemetryManagerForTest(
  specManager: ArmTelemetrySpecManager = new FakeArmTelemetrySpecManager(
    CPU_SPEC,
  ),
) {
  pmuRecordSection({} as RecordingManager, {} as App, specManager);
}

function createPmuProbeForTest(
  specManager: ArmTelemetrySpecManager = new FakeArmTelemetrySpecManager(
    CPU_SPEC,
  ),
  app: App = {} as App,
): RecordProbe {
  const subpage = pmuRecordSection({} as RecordingManager, app, specManager);
  if (subpage.kind !== 'PROBES_PAGE') {
    throw new Error('Expected PMU record section to be a probes page');
  }
  return subpage.probes[0];
}

function getPmuSettingForTest(probe = createPmuProbeForTest()): ProbeSetting {
  return probe.settings!.testSetting;
}

function validSerializedPmuConfig(
  configurationOverrides: Record<string, unknown> = {},
) {
  return {
    currentTabKey: 'tab-1',
    tabs: [
      {
        key: 'tab-1',
        configuration: {
          cpuSelected: '0x4114d',
          pmusSelected: ['CPU_CYCLES', 'INST_RETIRED'],
          cpuLeader: 'CPU_CYCLES',
          cpuLeaderUserSelected: true,
          cpuPerfType: '8',
          sampleByFrequency: true,
          frequency: 1000,
          period: 10000,
          captureCallstack: true,
          ...configurationOverrides,
        },
      },
    ],
  };
}

function genConfigForTest(serializedState: unknown): TraceConfigBuilder {
  const probe = createPmuProbeForTest();
  getPmuSettingForTest(probe).deserialize(serializedState);
  const tc = new TraceConfigBuilder();
  probe.genConfig(tc);
  return tc;
}

function dataSourceForTest(tc: TraceConfigBuilder, name: string) {
  return tc.dataSources.get(`${name}undefined`)?.config;
}

function perfEventConfigForTest(tc: TraceConfigBuilder, index = 0) {
  return dataSourceForTest(tc, `linux.perf ${index}`)?.perfEventConfig;
}

function mutableStateForTest(setting: ProbeSetting): PerfConfigurationState {
  const vnode = setting.render() as unknown as {
    children: Array<{attrs: {state: PerfConfigurationState}}>;
  };
  return vnode.children[0].attrs.state;
}

function initializePmuTabForTest(setting: ProbeSetting) {
  const containerVnode = setting.render() as unknown as {
    children: Array<{
      tag: ComponentConstructorForTest<PerfConfigurationComponentForTest>;
      attrs: {
        state: PerfConfigurationState;
        app: App;
        recMgr: RecordingManager;
      };
    }>;
  };
  const perfConfigurationVnode = containerVnode.children[0];
  const perfConfiguration = new perfConfigurationVnode.tag();
  perfConfiguration.oninit({attrs: perfConfigurationVnode.attrs});
  const rendered = perfConfiguration.view({
    attrs: perfConfigurationVnode.attrs,
  });
  const tabsVnode = rendered.children[3] as {
    attrs: {
      tabs: Array<{
        content: {
          tag: PerfCpuConfigurationComponentConstructorForTest;
          attrs: {
            state: PerfCpuConfigurationState;
            app: App;
            recMgr: RecordingManager;
          };
        };
      }>;
    };
  };
  const perfCpuConfigurations: PmuCpuConfigurationComponentForTest[] =
    tabsVnode.attrs.tabs.map((tab) => {
      const perfCpuConfiguration = new tab.content.tag();
      perfCpuConfiguration.oninit({attrs: tab.content.attrs});
      return perfCpuConfiguration;
    });
  return {
    state: perfConfigurationVnode.attrs.state,
    perfConfiguration,
    perfCpuConfigurations,
  };
}

describe('PMU config persistence', () => {
  test('serializes all PMU tabs and settings', () => {
    const state: PerfConfigurationState = {
      currentTabKey: 'tab-2',
      tabs: [
        {
          key: 'tab-1',
          configuration: {
            cpuSelected: '0x4114d',
            pmusSelected: ['CPU_CYCLES'],
            onCpuSelected: () => {},
            cpuLeader: 'CPU_CYCLES',
            cpuLeaderUserSelected: false,
            targetCpus: '0,2-3',
            cpuPerfType: '8',
            sampleByFrequency: false,
            period: 5000,
            frequency: 100,
            captureCallstack: true,
          },
        },
        {
          key: 'tab-2',
          configuration: {
            cpuSelected: undefined,
            pmusSelected: [],
            onCpuSelected: undefined,
            cpuLeader: undefined,
            cpuLeaderUserSelected: false,
            targetCpus: undefined,
            cpuPerfType: undefined,
            sampleByFrequency: true,
            period: 10000,
            frequency: 100,
            captureCallstack: false,
          },
        },
      ],
    };

    expect(serializePerfConfigurationState(state)).toEqual({
      currentTabKey: 'tab-2',
      tabs: [
        {
          key: 'tab-1',
          configuration: {
            cpuSelected: '0x4114d',
            pmusSelected: ['CPU_CYCLES'],
            cpuLeader: 'CPU_CYCLES',
            cpuLeaderUserSelected: false,
            targetCpus: '0,2-3',
            cpuPerfType: '8',
            sampleByFrequency: false,
            period: 5000,
            frequency: 100,
            captureCallstack: true,
          },
        },
        {
          key: 'tab-2',
          configuration: {
            cpuSelected: undefined,
            pmusSelected: [],
            cpuLeader: undefined,
            cpuLeaderUserSelected: false,
            targetCpus: undefined,
            cpuPerfType: undefined,
            sampleByFrequency: true,
            period: 10000,
            frequency: 100,
            captureCallstack: false,
          },
        },
      ],
    });
  });

  test('deserializes with validation and defaults', () => {
    setTelemetryManagerForTest();
    const state: PerfConfigurationState = {
      currentTabKey: undefined,
      tabs: [],
    };

    deserializePerfConfigurationState(state, {
      currentTabKey: 'tab-1',
      tabs: [
        {
          key: 'tab-1',
          configuration: {
            cpuSelected: '0x4114d',
            pmusSelected: ['CPU_CYCLES', 'DOES_NOT_EXIST', 'INST_RETIRED'],
            cpuLeader: 'DOES_NOT_EXIST',
            cpuLeaderUserSelected: true,
            targetCpus: '0,2-3',
            cpuPerfType: '8',
            sampleByFrequency: false,
            period: 5000,
            frequency: 1000,
            captureCallstack: true,
          },
        },
        {
          key: 'tab-2',
          configuration: {
            cpuSelected: 'missing-cpu',
            pmusSelected: ['CPU_CYCLES'],
            cpuLeader: 'CPU_CYCLES',
            cpuLeaderUserSelected: false,
            targetCpus: '1,,3',
            cpuPerfType: '-1',
            sampleByFrequency: 'yes',
            period: 0,
            frequency: -1,
            captureCallstack: 'no',
          },
        },
        {
          key: 'tab-3',
          configuration: {
            cpuSelected: '0x4114d',
            pmusSelected: ['CPU_CYCLES', 'INST_RETIRED'],
            cpuLeader: 'INST_RETIRED',
          },
        },
      ],
    });

    expect(state).toEqual({
      currentTabKey: 'tab-1',
      tabs: [
        {
          key: 'tab-1',
          configuration: {
            cpuSelected: '0x4114d',
            pmusSelected: ['CPU_CYCLES', 'INST_RETIRED'],
            onCpuSelected: undefined,
            cpuLeader: 'CPU_CYCLES',
            cpuLeaderUserSelected: true,
            targetCpus: '0,2-3',
            cpuPerfType: '8',
            sampleByFrequency: false,
            period: 5000,
            frequency: 1000,
            captureCallstack: true,
          },
        },
        {
          key: 'tab-2',
          configuration: {
            cpuSelected: undefined,
            pmusSelected: [],
            onCpuSelected: undefined,
            cpuLeader: undefined,
            cpuLeaderUserSelected: false,
            targetCpus: undefined,
            cpuPerfType: undefined,
            sampleByFrequency: true,
            period: 10000,
            frequency: 100,
            captureCallstack: false,
          },
        },
        {
          key: 'tab-3',
          configuration: {
            cpuSelected: '0x4114d',
            pmusSelected: ['CPU_CYCLES', 'INST_RETIRED'],
            onCpuSelected: undefined,
            cpuLeader: 'INST_RETIRED',
            cpuLeaderUserSelected: false,
            targetCpus: undefined,
            cpuPerfType: undefined,
            sampleByFrequency: true,
            period: 10000,
            frequency: 100,
            captureCallstack: false,
          },
        },
      ],
    });
  });

  test('drops PMU selection when a registered CPU descriptor is unavailable after refresh', () => {
    setTelemetryManagerForTest(new FakeArmTelemetrySpecManager(undefined));
    const state: PerfConfigurationState = {
      currentTabKey: undefined,
      tabs: [],
    };

    deserializePerfConfigurationState(state, {
      currentTabKey: 'tab-1',
      tabs: [
        {
          key: 'tab-1',
          configuration: {
            cpuSelected: '0x4114d',
            pmusSelected: ['CPU_CYCLES', 'INST_RETIRED'],
            cpuLeader: 'CPU_CYCLES',
            cpuLeaderUserSelected: true,
            targetCpus: '0,2-3',
            cpuPerfType: '8',
            sampleByFrequency: false,
            period: 5000,
            frequency: 1000,
            captureCallstack: true,
          },
        },
      ],
    });

    expect(state).toEqual({
      currentTabKey: 'tab-1',
      tabs: [
        {
          key: 'tab-1',
          configuration: {
            cpuSelected: undefined,
            pmusSelected: [],
            onCpuSelected: undefined,
            cpuLeader: undefined,
            cpuLeaderUserSelected: false,
            targetCpus: '0,2-3',
            cpuPerfType: '8',
            sampleByFrequency: false,
            period: 5000,
            frequency: 1000,
            captureCallstack: true,
          },
        },
      ],
    });
  });

  test('refreshes PMU tabs when the selected CPU spec is replaced', () => {
    expect(getCpuId(CPU_SPEC_1)).toEqual(getCpuId(CPU_SPEC));
    const specManager = new FakeArmTelemetrySpecManager(CPU_SPEC);
    const app = createFakeAppForTest();
    const probe = createPmuProbeForTest(specManager, app);
    const setting = getPmuSettingForTest(probe);
    setting.deserialize({
      currentTabKey: 'tab-1',
      tabs: [
        validSerializedPmuConfig().tabs[0],
        {
          key: 'tab-2',
          configuration: {
            cpuSelected: '0x4114d',
            pmusSelected: ['INST_RETIRED'],
            cpuLeader: 'INST_RETIRED',
            cpuLeaderUserSelected: true,
            cpuPerfType: '8',
            sampleByFrequency: true,
            frequency: 1000,
            period: 10000,
            captureCallstack: false,
          },
        },
      ],
    });

    const {state, perfCpuConfigurations} = initializePmuTabForTest(setting);
    expect(
      perfCpuConfigurations.map((component) => component.controller.cpus),
    ).toEqual([
      new Map([['0x4114d', 'Test CPU']]),
      new Map([['0x4114d', 'Test CPU']]),
    ]);
    expect(
      perfCpuConfigurations.map((component) =>
        component.controller.pmuOptions.map((option) => option.id),
      ),
    ).toEqual([
      ['CPU_CYCLES', 'INST_RETIRED'],
      ['CPU_CYCLES', 'INST_RETIRED'],
    ]);

    specManager.update(CPU_SPEC_1);

    expect(
      state.tabs.map((tab) => ({
        cpuSelected: tab.configuration.cpuSelected,
        pmusSelected: tab.configuration.pmusSelected,
        cpuLeader: tab.configuration.cpuLeader,
        cpuLeaderUserSelected: tab.configuration.cpuLeaderUserSelected,
      })),
    ).toEqual([
      {
        cpuSelected: '0x4114d',
        pmusSelected: [],
        cpuLeader: undefined,
        cpuLeaderUserSelected: false,
      },
      {
        cpuSelected: '0x4114d',
        pmusSelected: [],
        cpuLeader: undefined,
        cpuLeaderUserSelected: false,
      },
    ]);
    expect(
      perfCpuConfigurations.map((component) => component.controller.cpus),
    ).toEqual([
      new Map([['0x4114d', 'Updated Test CPU']]),
      new Map([['0x4114d', 'Updated Test CPU']]),
    ]);
    expect(
      perfCpuConfigurations.map((component) =>
        component.controller.pmuOptions.map((option) => option.id),
      ),
    ).toEqual([
      ['BR_RETIRED', 'L1D_CACHE_REFILL'],
      ['BR_RETIRED', 'L1D_CACHE_REFILL'],
    ]);
    expect(app.raf.scheduleFullRedraw).toHaveBeenCalled();
  });

  test('refreshes CPU lists when a new CPU spec is added', () => {
    expect(getCpuId(CPU_SPEC_2)).not.toEqual(getCpuId(CPU_SPEC));
    const specManager = new FakeArmTelemetrySpecManager(CPU_SPEC);
    const app = createFakeAppForTest();
    const probe = createPmuProbeForTest(specManager, app);
    const setting = getPmuSettingForTest(probe);
    setting.deserialize(validSerializedPmuConfig());

    const {state, perfCpuConfigurations} = initializePmuTabForTest(setting);
    expect(perfCpuConfigurations[0].controller.cpus).toEqual(
      new Map([['0x4114d', 'Test CPU']]),
    );

    specManager.add(CPU_SPEC_2);

    expect(state.tabs[0].configuration).toEqual({
      cpuSelected: '0x4114d',
      pmusSelected: ['CPU_CYCLES', 'INST_RETIRED'],
      onCpuSelected: expect.any(Function),
      cpuLeader: 'CPU_CYCLES',
      cpuLeaderUserSelected: true,
      targetCpus: undefined,
      cpuPerfType: '8',
      sampleByFrequency: true,
      period: 10000,
      frequency: 1000,
      captureCallstack: true,
    });
    expect(perfCpuConfigurations[0].controller.cpus).toEqual(
      new Map([
        ['0x4114d', 'Test CPU'],
        [getCpuId(CPU_SPEC_2), 'Added Test CPU'],
      ]),
    );
    expect(
      perfCpuConfigurations[0].controller.pmuOptions.map((option) => option.id),
    ).toEqual(['CPU_CYCLES', 'INST_RETIRED']);
    expect(app.raf.scheduleFullRedraw).toHaveBeenCalled();
  });

  test('clears PMU tabs when all CPU specs are cleared', () => {
    const specManager = new FakeArmTelemetrySpecManager(CPU_SPEC);
    const app = createFakeAppForTest();
    const probe = createPmuProbeForTest(specManager, app);
    const setting = getPmuSettingForTest(probe);
    setting.deserialize({
      currentTabKey: 'tab-1',
      tabs: [
        validSerializedPmuConfig().tabs[0],
        {
          key: 'tab-2',
          configuration: {
            cpuSelected: '0x4114d',
            pmusSelected: ['INST_RETIRED'],
            cpuLeader: 'INST_RETIRED',
            cpuLeaderUserSelected: true,
            cpuPerfType: '8',
            sampleByFrequency: true,
            frequency: 1000,
            period: 10000,
            captureCallstack: false,
          },
        },
      ],
    });

    const {state, perfCpuConfigurations} = initializePmuTabForTest(setting);
    expect(perfCpuConfigurations[0].controller.cpus).toEqual(
      new Map([['0x4114d', 'Test CPU']]),
    );

    specManager.clear();

    expect(
      state.tabs.map((tab) => ({
        cpuSelected: tab.configuration.cpuSelected,
        pmusSelected: tab.configuration.pmusSelected,
        cpuLeader: tab.configuration.cpuLeader,
        cpuLeaderUserSelected: tab.configuration.cpuLeaderUserSelected,
      })),
    ).toEqual([
      {
        cpuSelected: undefined,
        pmusSelected: [],
        cpuLeader: undefined,
        cpuLeaderUserSelected: false,
      },
      {
        cpuSelected: undefined,
        pmusSelected: [],
        cpuLeader: undefined,
        cpuLeaderUserSelected: false,
      },
    ]);
    expect(
      perfCpuConfigurations.map((component) => component.controller.cpus),
    ).toEqual([new Map(), new Map()]);
    expect(app.raf.scheduleFullRedraw).toHaveBeenCalled();
  });

  test('auto leader prefers CPU_CYCLES unless the user picked a leader', () => {
    expect(
      normalizeCpuLeader('INST_RETIRED', ['CPU_CYCLES', 'INST_RETIRED'], false),
    ).toEqual('CPU_CYCLES');

    expect(
      normalizeCpuLeader('INST_RETIRED', ['CPU_CYCLES', 'INST_RETIRED'], true),
    ).toEqual('INST_RETIRED');

    expect(normalizeCpuLeader(undefined, ['INST_RETIRED'], false)).toEqual(
      'INST_RETIRED',
    );
  });

  test('auto leader handles stale and empty selections', () => {
    expect(normalizeCpuLeader('MISSING', ['INST_RETIRED'], true)).toEqual(
      'INST_RETIRED',
    );

    expect(normalizeCpuLeader('CPU_CYCLES', [], true)).toBeUndefined();

    expect(normalizeCpuLeader('   ', ['CPU_CYCLES'], true)).toEqual(
      'CPU_CYCLES',
    );
  });

  test('deserializes duplicate tab keys with unique replacements', () => {
    setTelemetryManagerForTest();
    const state: PerfConfigurationState = {
      currentTabKey: undefined,
      tabs: [],
    };

    deserializePerfConfigurationState(state, {
      currentTabKey: 'tab-1',
      tabs: [
        {key: 'tab-1', configuration: {}},
        {key: 'tab-1', configuration: {}},
      ],
    });

    expect(state.currentTabKey).toEqual('tab-1');
    expect(state.tabs[0].key).toEqual('tab-1');
    expect(state.tabs[1].key).not.toEqual('tab-1');
    expect(new Set(state.tabs.map((tab) => tab.key)).size).toEqual(2);
  });

  test('deserializes invalid current tab key with first-tab fallback', () => {
    setTelemetryManagerForTest();
    const state: PerfConfigurationState = {
      currentTabKey: undefined,
      tabs: [],
    };

    deserializePerfConfigurationState(state, {
      currentTabKey: 'missing-tab',
      tabs: [
        {key: 'tab-1', configuration: {}},
        {key: 'tab-2', configuration: {}},
      ],
    });

    expect(state.currentTabKey).toEqual('tab-1');
  });

  test('deserializes non-object or missing tabs by clearing state', () => {
    setTelemetryManagerForTest();
    const state: PerfConfigurationState = {
      currentTabKey: 'tab-1',
      tabs: [
        {
          key: 'tab-1',
          configuration: {
            cpuSelected: '0x4114d',
            pmusSelected: ['CPU_CYCLES'],
            onCpuSelected: undefined,
            cpuLeader: 'CPU_CYCLES',
            cpuLeaderUserSelected: false,
            targetCpus: undefined,
            cpuPerfType: '8',
            sampleByFrequency: true,
            period: 10000,
            frequency: 100,
            captureCallstack: false,
          },
        },
      ],
    };

    deserializePerfConfigurationState(state, undefined);
    expect(state).toEqual({currentTabKey: undefined, tabs: []});

    deserializePerfConfigurationState(state, {currentTabKey: 'tab-1'});
    expect(state).toEqual({currentTabKey: undefined, tabs: []});
  });
});

describe('PMU config generation', () => {
  test('genConfig emits frequency-based PMU config', () => {
    const tc = genConfigForTest(validSerializedPmuConfig());

    expect(dataSourceForTest(tc, 'linux.system_info')).toBeDefined();
    expect(dataSourceForTest(tc, 'linux.perf 0')).toBeDefined();

    const perfConf = perfEventConfigForTest(tc)!;
    expect(perfConf.timebase).toEqual({
      name: 'CPU_CYCLES',
      rawEvent: {
        type: 8,
        config: 0x11,
      },
      frequency: 1000,
    });
    expect(perfConf.followers).toEqual([
      {
        name: 'INST_RETIRED',
        rawEvent: {
          type: 8,
          config: 0x08,
        },
      },
    ]);
    expect(perfConf.callstackSampling).toEqual({});
  });

  test('genConfig emits period-based PMU config', () => {
    const tc = genConfigForTest(
      validSerializedPmuConfig({
        sampleByFrequency: false,
        period: 5000,
      }),
    );

    const timebase = perfEventConfigForTest(tc)!.timebase!;
    expect(timebase.period).toEqual(5000);
    expect(timebase.frequency).toBeUndefined();
  });

  test('genConfig uses targetCpu when targetCpus is configured', () => {
    const tc = genConfigForTest(
      validSerializedPmuConfig({
        targetCpus: '0,2-3',
      }),
    );

    const perfConf = perfEventConfigForTest(tc)!;
    expect(perfConf.targetCpu).toEqual([0, 2, 3]);
    expect(perfConf.cpuid).toBeUndefined();
  });

  test('genConfig falls back to cpuid when targetCpus is omitted', () => {
    const tc = genConfigForTest(validSerializedPmuConfig());

    const perfConf = perfEventConfigForTest(tc)!;
    expect(perfConf.cpuid).toEqual(['4114d']);
  });

  test('genConfig skips incomplete tabs', () => {
    const probe = createPmuProbeForTest();
    const setting = getPmuSettingForTest(probe);
    const state = mutableStateForTest(setting);
    state.tabs = [
      {
        key: 'missing-cpu',
        configuration: {
          cpuSelected: undefined,
          pmusSelected: ['CPU_CYCLES'],
          onCpuSelected: undefined,
          cpuLeader: 'CPU_CYCLES',
          cpuLeaderUserSelected: false,
          targetCpus: undefined,
          cpuPerfType: '8',
          sampleByFrequency: true,
          period: 10000,
          frequency: 1000,
          captureCallstack: false,
        },
      },
      {
        key: 'empty-pmus',
        configuration: {
          cpuSelected: '0x4114d',
          pmusSelected: [],
          onCpuSelected: undefined,
          cpuLeader: 'CPU_CYCLES',
          cpuLeaderUserSelected: false,
          targetCpus: undefined,
          cpuPerfType: '8',
          sampleByFrequency: true,
          period: 10000,
          frequency: 1000,
          captureCallstack: false,
        },
      },
      {
        key: 'missing-leader',
        configuration: {
          cpuSelected: '0x4114d',
          pmusSelected: ['CPU_CYCLES'],
          onCpuSelected: undefined,
          cpuLeader: undefined,
          cpuLeaderUserSelected: false,
          targetCpus: undefined,
          cpuPerfType: '8',
          sampleByFrequency: true,
          period: 10000,
          frequency: 1000,
          captureCallstack: false,
        },
      },
    ];

    const tc = new TraceConfigBuilder();
    probe.genConfig(tc);

    expect(perfEventConfigForTest(tc)).toBeUndefined();
    expect(perfEventConfigForTest(tc, 1)).toBeUndefined();
    expect(perfEventConfigForTest(tc, 2)).toBeUndefined();
  });

  test.each([
    [
      'invalid perf CPU event type',
      (state: PerfConfigurationState) => {
        state.tabs[0].configuration.cpuPerfType = '0';
      },
      'PMU Configuration Error: Invalid perf CPU event type',
    ],
    [
      'invalid sampling frequency',
      (state: PerfConfigurationState) => {
        state.tabs[0].configuration.frequency = 0;
      },
      'PMU Configuration Error: Invalid sampling frequency',
    ],
    [
      'invalid sampling period',
      (state: PerfConfigurationState) => {
        state.tabs[0].configuration.sampleByFrequency = false;
        state.tabs[0].configuration.period = 0;
      },
      'PMU Configuration Error: Invalid sampling period',
    ],
    [
      'invalid target CPUs',
      (state: PerfConfigurationState) => {
        state.tabs[0].configuration.targetCpus = '1,,3';
      },
      'PMU Configuration Error: Invalid target CPU value',
    ],
  ])('genConfig rejects %s', (_name, mutateState, alertMessage) => {
    const probe = createPmuProbeForTest();
    const setting = getPmuSettingForTest(probe);
    setting.deserialize(validSerializedPmuConfig());
    mutateState(mutableStateForTest(setting));

    const alert = vi.fn();
    vi.stubGlobal('alert', alert);
    const tc = new TraceConfigBuilder();
    try {
      probe.genConfig(tc);
    } finally {
      vi.unstubAllGlobals();
    }

    expect(alert).toHaveBeenCalledWith(alertMessage);
    expect(perfEventConfigForTest(tc)).toBeUndefined();
  });
});
