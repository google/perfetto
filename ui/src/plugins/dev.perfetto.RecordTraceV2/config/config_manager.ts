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

import protos from '../../../protos';
import {assertExists, assertFalse, assertTrue} from '../../../base/logging';
import {getOrCreate} from '../../../base/utils';
import {ProbesSchema} from '../serialization_schema';
import {TargetPlatformId} from '../interfaces/target_platform';
import {RecordProbe, supportsPlatform} from './config_interfaces';
import {DEFAULT_BUFFER_ID, TraceConfigBuilder} from './trace_config_builder';

/**
 * ConfigManager holds all the state required for config generation (everything
 * in the record page that has nothing to do with the actual record over
 * webusb/websocket).
 * Recording is arranged as a set of Probes. A Probe is a slightly different
 * concept than a DataSource, as in, it's a higher-level, more user-friendly
 * concept to help the user configuring tracing behaviours with toggles.
 * In some cases a Probe can just match 1:1 with a data source; in other cases
 * N probes can contribute to the same data source (e.g. when they enable
 * different ftrace events); In other cases a probe can enable 2+ data sources.
 * At the end of the day, probe contribute to generating a TraceConfig proto.
 * They do so in a react-style fashion (we start from blank and append entries
 * every time there is a change). @see {@link TraceConfigBuilder}.
 */
export class ConfigManager {
  readonly probesById = new Map<string, RecordProbe>();
  private _traceConfig = new TraceConfigBuilder();
  private enabledProbes = new Map<string, boolean>();
  private indirectlyEnabledProbes = new Map<string, Set<string>>();
  private _generation = 0;

  constructor() {}

  get generation() {
    return this._generation;
  }

  get traceConfig() {
    return this._traceConfig;
  }

  registerProbes(probes: ReadonlyArray<RecordProbe>) {
    for (const probe of probes) {
      assertFalse(this.probesById.has(probe.id));
      this.probesById.set(probe.id, probe);
    }
  }

  setProbeEnabled(probeId: string, enabled: boolean) {
    const probe = assertExists(this.probesById.get(probeId));
    this.enabledProbes.set(probeId, enabled);
    for (const depProbeId of probe.dependencies ?? []) {
      assertTrue(this.probesById.has(depProbeId));
      const depSet = getOrCreate(
        this.indirectlyEnabledProbes,
        depProbeId,
        () => new Set<string>(),
      );
      if (enabled) {
        depSet.add(probeId);
      } else {
        depSet.delete(probeId);
      }
    }
    // Notify that probe settings changed
    this._generation++;
  }

  isProbeEnabled(probeId: string): boolean {
    const directlyEnabled = this.enabledProbes.get(probeId) === true;
    const enabledDueToDeps = Boolean(
      this.indirectlyEnabledProbes.get(probeId)?.size,
    );
    return directlyEnabled || enabledDueToDeps;
  }

  hasActiveProbes(): boolean {
    for (const probeId of this.probesById.keys()) {
      if (this.isProbeEnabled(probeId)) {
        return true;
      }
    }
    return false;
  }

  /**
   * Returns the human-friendly name for the probes that are enabled and depend
   * on this probe. This is so we can tell the user: you cannot turn this probe
   * off because another probe you enbled requires this one.
   */
  getProbeEnableDependants(probeId: string): string[] {
    return Array.from(this.indirectlyEnabledProbes.get(probeId) ?? []).map(
      (id) => assertExists(this.probesById.get(id)).title,
    );
  }

  /**
   * Generates the TraceConfig proto for the current configuration.
   */
  genTraceConfig(platform: TargetPlatformId): protos.TraceConfig {
    // We approach trace config generation similar to vdom rendering: we start
    // fresh all the time and let the various probes add things to the
    // TraceConfigBuilder.

    this._traceConfig.dataSources.clear();

    // Clear all buffers other than the default one.
    for (const bufId of this._traceConfig.buffers.keys()) {
      if (bufId !== DEFAULT_BUFFER_ID) {
        this._traceConfig.buffers.delete(bufId);
      }
    }

    // Now regenerate the config. Go in probe registration order, but
    // respect dependencies (deps come first).
    const orderedProbes = this.getProbesOrderedByDep(/* enabledOnly */ true);

    for (const probe of orderedProbes) {
      if (!supportsPlatform(probe, platform)) continue;
      probe.genConfig(this._traceConfig);
    }
    return this._traceConfig.toTraceConfig();
  }

  // For sharing and localstorage persistence.
  serializeProbes(): ProbesSchema {
    return Object.fromEntries(
      this.getProbesOrderedByDep(/* enabledOnly */ true).map((probe) => [
        probe.id,
        {
          settings: Object.fromEntries(
            Object.entries(probe.settings ?? {}).map(([settingId, setting]) => [
              settingId,
              setting.serialize(),
            ]),
          ),
        },
      ]),
    );
  }

  // For sharing and localstorage persistence.
  deserializeProbes(state: ProbesSchema): void {
    this.enabledProbes.clear();
    this.indirectlyEnabledProbes.clear();
    this.getProbesOrderedByDep().forEach((probe) => {
      const probeState = state[probe.id];
      if (probeState === undefined || probeState.settings === undefined) {
        return;
      }
      this.setProbeEnabled(probe.id, true);
      if (probe.settings === undefined) {
        // The probe has no settings, there is nothing to restore.
        // This return is theoretically redundant but is here to make tsc happy.
        return;
      }
      for (const [key, settingState] of Object.entries(probeState.settings)) {
        if (key in probe.settings) {
          probe.settings[key].deserialize(settingState);
        }
      }
    });
  }

  private getProbesOrderedByDep(enabledOnly = false): RecordProbe[] {
    const orderedProbes: RecordProbe[] = [];
    const seenIds = new Set<string>();
    const queueProbe = (probeId: string) => {
      if (enabledOnly && !this.isProbeEnabled(probeId)) return;
      const probe = assertExists(this.probesById.get(probeId));
      if (orderedProbes.includes(probe)) return; // Already added.
      if (seenIds.has(probeId)) {
        throw new Error('Cycle detected in probe ' + probeId);
      }
      seenIds.add(probeId);
      for (const dep of probe.dependencies ?? []) {
        queueProbe(dep);
      }
      orderedProbes.push(probe);
    };
    for (const probeId of this.probesById.keys()) {
      queueProbe(probeId);
    }
    return orderedProbes;
  }
}
