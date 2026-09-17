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

import {errResult} from '../../../base/result';
import {ConfigManager} from './config_manager';
import type {RecordProbe, RecordSubpage} from './config_interfaces';
import {ADV_FTRACE_PROBE_ID, advancedRecordSection} from '../pages/advanced';
import {androidRecordSection} from '../pages/android';
import {chromeRecordSection} from '../pages/chrome';
import {cpuRecordSection} from '../pages/cpu';
import {gpuRecordSection} from '../pages/gpu';
import {linuxRecordSection} from '../pages/linux';
import {memoryRecordSection} from '../pages/memory';
import {networkRecordSection} from '../pages/network';
import {perfettoSDKRecordSection} from '../pages/perfetto_sdk';
import {powerRecordSection} from '../pages/power';
import {stackSamplingRecordSection} from '../pages/stack_sampling';
import {
  ANDROID_PRESETS,
  CHROME_PRESETS,
  LINUX_PRESETS,
  type Preset,
} from '../presets';
import {
  PROBES_SESSION_SCHEMA,
  type ProbesSchema,
} from '../serialization_schema';

// All the probe pages of the record page, i.e. everything that can hold a
// setting. New pages should be added here so that they are covered by the
// "loading a config resets everything" test below.
function allProbePages(): RecordSubpage[] {
  return [
    chromeRecordSection(
      async () => errResult('No extension in tests'),
      () => 'CHROME',
    ),
    cpuRecordSection(),
    gpuRecordSection(),
    powerRecordSection(),
    memoryRecordSection(),
    linuxRecordSection(),
    androidRecordSection(),
    perfettoSDKRecordSection(),
    stackSamplingRecordSection(),
    networkRecordSection(),
    advancedRecordSection(),
  ];
}

function newConfigManager(): ConfigManager {
  const cfgMgr = new ConfigManager();
  for (const page of allProbePages()) {
    if (page.kind !== 'PROBES_PAGE') continue;
    cfgMgr.registerProbes(page.probes);
  }
  return cfgMgr;
}

// The value of every setting of every probe, INCLUDING the disabled ones.
// ConfigManager.serializeProbes() only looks at the enabled probes, so it
// can't see a stale value hiding in a probe that is currently off (which
// would pop back up the moment the user re-enables it).
function dumpAllSettings(cfgMgr: ConfigManager): Record<string, unknown> {
  const dump: Record<string, unknown> = {};
  for (const probe of cfgMgr.probesById.values()) {
    const settings: Record<string, unknown> = {};
    for (const [id, setting] of Object.entries(probe.settings ?? {})) {
      settings[id] = setting.serialize();
    }
    dump[probe.id] = settings;
  }
  return dump;
}

// Returns a value that is different from `value` but of a plausible shape, so
// that widgets don't just reject it and fall back on their default.
function perturb(value: unknown): unknown {
  if (typeof value === 'boolean') return !value;
  if (typeof value === 'number') return value + 4321;
  if (typeof value === 'string') return value + '_perturbed';
  if (Array.isArray(value)) return [...value, 'perturbed'];
  if (typeof value === 'object' && value !== null) {
    return Object.fromEntries(
      Object.entries(value as Record<string, unknown>).map(([k, v]) => [
        k,
        perturb(v),
      ]),
    );
  }
  return 'perturbed';
}

// A config that enables every probe and gives every setting a value that
// differs from the one it currently holds.
function everythingPerturbed(cfgMgr: ConfigManager): ProbesSchema {
  const probes: ProbesSchema = {};
  for (const probe of cfgMgr.probesById.values()) {
    const settings: Record<string, unknown> = {};
    for (const [id, setting] of Object.entries(probe.settings ?? {})) {
      settings[id] = perturb(setting.serialize());
    }
    probes[probe.id] = {settings};
  }
  return probes;
}

function allPresets(): Preset[] {
  return [...ANDROID_PRESETS, ...LINUX_PRESETS, ...CHROME_PRESETS];
}

function probesOf(preset: Preset): ProbesSchema {
  return PROBES_SESSION_SCHEMA.parse(preset.session).probes;
}

function ftraceProbe(cfgMgr: ConfigManager): RecordProbe {
  const probe = cfgMgr.probesById.get(ADV_FTRACE_PROBE_ID);
  if (probe === undefined) throw new Error('No advanced ftrace probe');
  return probe;
}

// The FtraceConfig of the TraceConfig generated for the current settings.
function genFtraceConfig(cfgMgr: ConfigManager) {
  for (const ds of cfgMgr.genTraceConfig('ANDROID').dataSources ?? []) {
    const ftraceConfig = ds.config?.ftraceConfig;
    if (ftraceConfig !== undefined && ftraceConfig !== null) {
      return ftraceConfig as {symbolizeKsyms?: boolean; bufferSizeKb?: number};
    }
  }
  throw new Error('The generated TraceConfig has no ftrace data source');
}

describe('ConfigManager', () => {
  // Regression test for #7347: the ftrace buffer size and the "resolve kernel
  // symbols" toggle survived the re-selection of the default config, because
  // the advanced ftrace probe is not part of any preset (it's pulled in as a
  // dependency of cpu_sched) and nothing restored its settings.
  it('restores the settings of the probes not in the loaded config', () => {
    const cfgMgr = newConfigManager();
    const defaultPreset = ANDROID_PRESETS[0];
    cfgMgr.deserializeProbes(probesOf(defaultPreset));
    const pristine = dumpAllSettings(cfgMgr);

    // Turn off "Resolve kernel symbols" and set an ftrace buffer size, as the
    // user would in the "Advanced ftrace config" page.
    const settings = ftraceProbe(cfgMgr).settings ?? {};
    (settings['ksyms'] as unknown as {setEnabled(v: boolean): void}).setEnabled(
      false,
    );
    (settings['bufSize'] as unknown as {setValue(v: number): void}).setValue(
      4 * 1024,
    );
    expect(dumpAllSettings(cfgMgr)).not.toEqual(pristine);
    expect(genFtraceConfig(cfgMgr).symbolizeKsyms ?? undefined).toEqual(
      undefined,
    );
    expect(genFtraceConfig(cfgMgr).bufferSizeKb).toEqual(4 * 1024);

    // Re-select the default config: everything must go back to how it was.
    cfgMgr.deserializeProbes(probesOf(defaultPreset));
    expect(dumpAllSettings(cfgMgr)).toEqual(pristine);

    // ...and that must show up in the generated TraceConfig too.
    expect(genFtraceConfig(cfgMgr).symbolizeKsyms).toEqual(true);
    expect(genFtraceConfig(cfgMgr).bufferSizeKb ?? undefined).toEqual(
      undefined,
    );
  });

  // The general form of the test above: whatever the record page was showing
  // before, loading a config must land on exactly the same state as loading it
  // on a brand new record page. This covers every probe and every setting,
  // including the ones added after this test was written.
  it('loads a config the same way regardless of the previous state', () => {
    for (const preset of allPresets()) {
      const reference = newConfigManager();
      reference.deserializeProbes(probesOf(preset));
      const expected = dumpAllSettings(reference);

      // 1. From a config where every single setting has been changed.
      const cfgMgr = newConfigManager();
      cfgMgr.deserializeProbes(everythingPerturbed(cfgMgr));
      cfgMgr.deserializeProbes(probesOf(preset));
      expect(dumpAllSettings(cfgMgr)).toEqual(expected);

      // 2. From each one of the other presets.
      for (const other of allPresets()) {
        const fromOther = newConfigManager();
        fromOther.deserializeProbes(probesOf(other));
        fromOther.deserializeProbes(probesOf(preset));
        expect(dumpAllSettings(fromOther)).toEqual(expected);
      }
    }
  });

  it('clears every setting when loading an empty config', () => {
    const pristine = dumpAllSettings(newConfigManager());
    const cfgMgr = newConfigManager();
    cfgMgr.deserializeProbes(everythingPerturbed(cfgMgr));
    cfgMgr.deserializeProbes({});
    expect(dumpAllSettings(cfgMgr)).toEqual(pristine);
    expect(cfgMgr.hasActiveProbes()).toEqual(false);
  });
});
