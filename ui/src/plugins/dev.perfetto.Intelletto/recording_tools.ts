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

import {z} from 'zod';
import type {App} from '../../public/app';
import RecordTraceV2Plugin from '../dev.perfetto.RecordTraceV2';
import {supportsPlatform} from '../dev.perfetto.RecordTraceV2/config/config_interfaces';
import type {RecordingTarget} from '../dev.perfetto.RecordTraceV2/interfaces/recording_target';
import type {RecordingTargetProvider} from '../dev.perfetto.RecordTraceV2/interfaces/recording_target_provider';
import type {TargetPlatformId} from '../dev.perfetto.RecordTraceV2/interfaces/target_platform';
import {getPresetsForPlatform} from '../dev.perfetto.RecordTraceV2/presets';
import type {RecordingManager} from '../dev.perfetto.RecordTraceV2/recording_manager';
import type {ToolRegistry} from './tools';

const PLATFORM_SCHEMA = z.enum(['ANDROID', 'CHROME', 'CHROME_OS', 'LINUX']);

const MODE_SCHEMA = z.enum(['STOP_WHEN_FULL', 'RING_BUFFER', 'LONG_TRACE']);

interface DiscoveredTarget {
  readonly provider: RecordingTargetProvider;
  readonly target: RecordingTarget;
}

async function discoverTargets(
  recMgr: RecordingManager,
  platform: TargetPlatformId,
): Promise<DiscoveredTarget[]> {
  const discovered: DiscoveredTarget[] = [];
  for (const provider of recMgr.listProvidersForCurrentPlatform()) {
    try {
      const targets = await provider.listTargets(platform);
      for (const target of targets) {
        discovered.push({provider, target});
      }
    } catch {
      // Offline providers (e.g. ADB websocket not running) should not block
      // discovering targets from other providers.
    }
  }
  return discovered;
}

function summarizeConfig(recMgr: RecordingManager) {
  const platform = recMgr.currentPlatform;
  const tc = recMgr.recordConfig.traceConfig;
  const enabledProbes: string[] = [];
  for (const [id, probe] of recMgr.recordConfig.probesById) {
    if (
      supportsPlatform(probe, platform) &&
      recMgr.recordConfig.isProbeEnabled(id)
    ) {
      enabledProbes.push(id);
    }
  }
  return {
    platform,
    selectedConfigId: recMgr.selectedConfigId,
    selectedConfigName: recMgr.selectedConfigName,
    isConfigModified: recMgr.isConfigModified,
    mode: tc.mode,
    durationMs: tc.durationMs,
    bufSizeMb: tc.defaultBuffer.sizeKb / 1024,
    compression: tc.compression,
    enabledProbes,
  };
}

/**
 * Register app-scoped tools for discovering connected devices, configuring
 * recording presets/probes, and starting/stopping trace recordings via
 * RecordTraceV2.
 */
export function registerRecordingTools(tools: ToolRegistry, app: App): void {
  const getRecMgr = (): RecordingManager =>
    RecordTraceV2Plugin.getRecordingManager(app);

  tools.registerTool({
    name: 'list_recording_targets',
    description:
      'List connected devices/targets available for recording a trace (e.g. ' +
      'Android phones over WebUSB/ADB/WDP, Linux, Chrome). Optionally switch ' +
      'the target platform first.',
    shape: {
      platform: PLATFORM_SCHEMA.optional().describe(
        'Target platform to list devices for (defaults to the current platform, usually ANDROID).',
      ),
    },
    callback: async ({platform}) => {
      const recMgr = getRecMgr();
      if (platform !== undefined && platform !== recMgr.currentPlatform) {
        recMgr.setPlatform(platform);
      }
      const activePlatform = recMgr.currentPlatform;
      const discovered = await discoverTargets(recMgr, activePlatform);
      const result: Record<string, unknown> = {
        platform: activePlatform,
        currentProvider: recMgr.currentProvider?.id,
        currentTarget:
          recMgr.currentTarget !== undefined
            ? {
                id: recMgr.currentTarget.id,
                name: recMgr.currentTarget.name,
                connected: recMgr.currentTarget.connected,
              }
            : undefined,
        targets: discovered.map(({provider, target}) => ({
          providerId: provider.id,
          providerName: provider.name,
          targetId: target.id,
          name: target.name,
          connected: target.connected,
        })),
      };
      if (discovered.length === 0) {
        result.note =
          'No connected devices found. For WebUSB, the user must first click ' +
          '"Add new device" on the Record page (#!/record) since WebUSB ' +
          'pairing requires a user gesture.';
      }
      return JSON.stringify(result);
    },
  });

  tools.registerTool({
    name: 'select_recording_target',
    description:
      'Select a connected recording target by targetId (from ' +
      'list_recording_targets) so subsequent start_recording calls record ' +
      'from it.',
    mutating: true,
    shape: {
      targetId: z.string().describe('Target id from list_recording_targets.'),
      providerId: z
        .string()
        .optional()
        .describe('Optional provider id to disambiguate if multiple match.'),
      platform: PLATFORM_SCHEMA.optional().describe(
        'Optional platform to switch to before selecting.',
      ),
    },
    callback: async ({targetId, providerId, platform}) => {
      const recMgr = getRecMgr();
      if (platform !== undefined && platform !== recMgr.currentPlatform) {
        recMgr.setPlatform(platform);
      }
      const activePlatform = recMgr.currentPlatform;
      const discovered = await discoverTargets(recMgr, activePlatform);
      const match = discovered.find(
        (d) =>
          d.target.id === targetId &&
          (providerId === undefined || d.provider.id === providerId),
      );
      if (match === undefined) {
        throw new Error(
          `Target "${targetId}" not found for platform ${activePlatform}. ` +
            `Call list_recording_targets to see available devices.`,
        );
      }
      await recMgr.setProvider(match.provider);
      recMgr.setTarget(match.target);
      recMgr.persistIntoLocalStorage();
      app.raf.scheduleFullRedraw();
      return JSON.stringify({
        platform: activePlatform,
        providerId: match.provider.id,
        targetId: match.target.id,
        name: match.target.name,
        connected: match.target.connected,
      });
    },
  });

  tools.registerTool({
    name: 'list_recording_presets',
    description:
      'List built-in recording presets (e.g. default, battery, thermal, ' +
      'graphics) and user-saved configs for the current or given platform, ' +
      'plus the active recording configuration summary.',
    shape: {
      platform: PLATFORM_SCHEMA.optional().describe(
        'Platform to list presets for (defaults to current platform).',
      ),
    },
    callback: async ({platform}) => {
      const recMgr = getRecMgr();
      const targetPlatform = platform ?? recMgr.currentPlatform;
      const presets = getPresetsForPlatform(targetPlatform).map((p) => ({
        id: p.id,
        title: p.title,
        subtitle: p.subtitle,
      }));
      return JSON.stringify({
        platform: targetPlatform,
        presets,
        savedConfigs: recMgr.savedConfigs.map((c) => c.name),
        currentConfig: summarizeConfig(recMgr),
      });
    },
  });

  tools.registerTool({
    name: 'list_recording_probes',
    description:
      'List available recording probes for the current or given platform, ' +
      'including their ids, sections, descriptions, enabled state, ' +
      'dependencies, and current setting values.',
    shape: {
      platform: PLATFORM_SCHEMA.optional().describe(
        'Platform to filter probes for (defaults to current platform).',
      ),
      enabledOnly: z
        .boolean()
        .optional()
        .describe('If true, return only currently enabled probes.'),
    },
    callback: async ({platform, enabledOnly}) => {
      const recMgr = getRecMgr();
      const targetPlatform = platform ?? recMgr.currentPlatform;
      const probes: Array<Record<string, unknown>> = [];
      for (const page of recMgr.pages.values()) {
        if (page.kind !== 'PROBES_PAGE') continue;
        for (const probe of page.probes) {
          if (!supportsPlatform(probe, targetPlatform)) continue;
          const enabled = recMgr.recordConfig.isProbeEnabled(probe.id);
          if (enabledOnly === true && !enabled) continue;
          const settings: Record<string, unknown> = {};
          if (probe.settings !== undefined) {
            for (const [key, setting] of Object.entries(probe.settings)) {
              settings[key] = setting.serialize();
            }
          }
          probes.push({
            id: probe.id,
            section: page.title,
            title: probe.title,
            description: probe.description,
            enabled,
            ...(probe.dependencies !== undefined &&
            probe.dependencies.length > 0
              ? {dependencies: probe.dependencies}
              : {}),
            ...(Object.keys(settings).length > 0 ? {settings} : {}),
          });
        }
      }
      return JSON.stringify({
        platform: targetPlatform,
        probes,
      });
    },
  });

  tools.registerTool({
    name: 'configure_recording',
    description:
      'Configure what will be recorded using built-in presets, saved ' +
      'configs, individual probes, probe settings, and buffer/duration ' +
      'options. Call list_recording_presets or list_recording_probes first ' +
      'to discover valid ids.',
    mutating: true,
    shape: {
      presetId: z
        .string()
        .optional()
        .describe(
          'Built-in preset id (e.g. "default", "battery", "thermal", "graphics") ' +
            'or saved config name to load as the starting point.',
        ),
      clearProbes: z
        .boolean()
        .optional()
        .describe(
          'If true (and presetId is not set), clear all enabled probes first.',
        ),
      enableProbes: z
        .array(z.string())
        .optional()
        .describe('Probe ids to enable.'),
      disableProbes: z
        .array(z.string())
        .optional()
        .describe('Probe ids to disable.'),
      probeSettings: z
        .record(z.string(), z.record(z.string(), z.unknown()))
        .optional()
        .describe(
          'Map of probeId -> {settingKey: value} to update probe settings.',
        ),
      mode: MODE_SCHEMA.optional().describe(
        'Recording mode: STOP_WHEN_FULL, RING_BUFFER, or LONG_TRACE.',
      ),
      durationMs: z
        .number()
        .int()
        .positive()
        .optional()
        .describe(
          'Max recording duration in milliseconds (e.g. 10000 for 10s).',
        ),
      bufSizeMb: z
        .number()
        .positive()
        .optional()
        .describe('In-memory trace buffer size in MB (e.g. 64).'),
      compression: z
        .boolean()
        .optional()
        .describe('Whether to enable deflate compression on the trace.'),
    },
    callback: async ({
      presetId,
      clearProbes,
      enableProbes,
      disableProbes,
      probeSettings,
      mode,
      durationMs,
      bufSizeMb,
      compression,
    }) => {
      const recMgr = getRecMgr();
      const platform = recMgr.currentPlatform;

      if (presetId !== undefined) {
        const preset = getPresetsForPlatform(platform).find(
          (p) => p.id === presetId,
        );
        if (preset !== undefined) {
          recMgr.clearCustomTraceConfig();
          recMgr.loadConfig({
            config: preset.session,
            configId: `preset:${preset.id}`,
            configName: preset.title,
          });
        } else {
          const saved = recMgr.savedConfigs.find((c) => c.name === presetId);
          if (saved === undefined) {
            throw new Error(
              `Unknown preset or saved config "${presetId}" for platform ${platform}. ` +
                `Call list_recording_presets to see valid options.`,
            );
          }
          recMgr.clearCustomTraceConfig();
          recMgr.loadConfig({
            config: saved.config,
            configId: `saved:${saved.name}`,
            configName: saved.name,
          });
        }
      } else if (clearProbes === true) {
        recMgr.clearSession();
      } else if (recMgr.hasCustomTraceConfig) {
        recMgr.clearCustomTraceConfig();
      }

      for (const id of enableProbes ?? []) {
        const probe = recMgr.recordConfig.probesById.get(id);
        if (probe === undefined || !supportsPlatform(probe, platform)) {
          throw new Error(
            `Unknown or unsupported probe "${id}" for platform ${platform}. ` +
              `Call list_recording_probes to see valid probe ids.`,
          );
        }
        recMgr.recordConfig.setProbeEnabled(id, true);
      }

      for (const id of disableProbes ?? []) {
        const probe = recMgr.recordConfig.probesById.get(id);
        if (probe === undefined) {
          throw new Error(`Unknown probe "${id}".`);
        }
        recMgr.recordConfig.setProbeEnabled(id, false);
      }

      if (probeSettings !== undefined) {
        for (const [probeId, settingsMap] of Object.entries(probeSettings)) {
          const probe = recMgr.recordConfig.probesById.get(probeId);
          if (probe === undefined) {
            throw new Error(`Unknown probe "${probeId}" in probeSettings.`);
          }
          for (const [settingKey, value] of Object.entries(settingsMap)) {
            const setting = probe.settings?.[settingKey];
            if (setting === undefined) {
              throw new Error(
                `Unknown setting "${settingKey}" on probe "${probeId}".`,
              );
            }
            setting.deserialize(value);
          }
        }
      }

      const tc = recMgr.recordConfig.traceConfig;
      if (mode !== undefined) tc.mode = mode;
      if (durationMs !== undefined) tc.durationMs = durationMs;
      if (bufSizeMb !== undefined) {
        tc.defaultBuffer.sizeKb = Math.round(bufSizeMb * 1024);
      }
      if (compression !== undefined) tc.compression = compression;

      recMgr.persistIntoLocalStorage();
      app.raf.scheduleFullRedraw();
      return JSON.stringify(summarizeConfig(recMgr));
    },
  });

  tools.registerTool({
    name: 'start_recording',
    description:
      'Start recording a trace on the currently selected target using the ' +
      'current recording configuration. If no target is selected yet, ' +
      'automatically selects the first available connected device. When ' +
      'autoOpenTrace is true (default), the recorded trace opens ' +
      'automatically once recording finishes.',
    mutating: true,
    shape: {
      autoOpenTrace: z
        .boolean()
        .optional()
        .describe(
          'Automatically open the recorded trace in Perfetto when finished (default true).',
        ),
    },
    callback: async ({autoOpenTrace}) => {
      const recMgr = getRecMgr();
      if (recMgr.currentTarget === undefined) {
        const discovered = await discoverTargets(
          recMgr,
          recMgr.currentPlatform,
        );
        const first = discovered[0];
        if (first === undefined) {
          throw new Error(
            'No recording target connected. Ask the user to connect a device ' +
              '(for WebUSB, click "Add new device" on #!/record).',
          );
        }
        await recMgr.setProvider(first.provider);
        recMgr.setTarget(first.target);
      }
      if (
        !recMgr.hasCustomTraceConfig &&
        !recMgr.recordConfig.hasActiveProbes()
      ) {
        throw new Error(
          'No probes are enabled. Call configure_recording (e.g. with a ' +
            'presetId like "default") before starting a recording.',
        );
      }

      recMgr.autoOpenTraceWhenTracingEnds = autoOpenTrace ?? true;
      const session = await recMgr.startTracing();
      await session.started;

      return JSON.stringify({
        state: session.state,
        inProgress: session.inProgress,
        error: session.error,
        fileName: session.fileName,
        eta: session.eta,
        target: recMgr.currentTarget?.name,
        autoOpenTrace: recMgr.autoOpenTraceWhenTracingEnds,
      });
    },
  });

  tools.registerTool({
    name: 'get_recording_status',
    description:
      'Check the status of the current or most recent trace recording ' +
      'session (state, ETA, error, target).',
    shape: {},
    callback: async () => {
      const recMgr = getRecMgr();
      const session = recMgr.currentSession;
      if (session === undefined) {
        return JSON.stringify({
          active: false,
          target: recMgr.currentTarget?.name,
        });
      }
      return JSON.stringify({
        active: true,
        state: session.state,
        inProgress: session.inProgress,
        isCompleted: session.isCompleted,
        error: session.error,
        fileName: session.fileName,
        eta: session.eta,
        target: recMgr.currentTarget?.name,
      });
    },
  });

  tools.registerTool({
    name: 'stop_recording',
    description:
      'Stop an in-progress trace recording (flushes and opens the trace if ' +
      'autoOpenTrace is enabled), or cancel and discard it.',
    mutating: true,
    shape: {
      cancel: z
        .boolean()
        .optional()
        .describe(
          'If true, cancel and discard the recording instead of stopping cleanly.',
        ),
    },
    callback: async ({cancel}) => {
      const recMgr = getRecMgr();
      const current = recMgr.currentSession;
      if (
        current === undefined ||
        current.session === undefined ||
        !current.inProgress
      ) {
        throw new Error('No active recording session to stop.');
      }
      if (cancel === true) {
        await current.session.cancel();
      } else {
        await current.session.stop();
      }
      app.raf.scheduleFullRedraw();
      return JSON.stringify({
        state: current.state,
        inProgress: current.inProgress,
        fileName: current.fileName,
      });
    },
  });
}
