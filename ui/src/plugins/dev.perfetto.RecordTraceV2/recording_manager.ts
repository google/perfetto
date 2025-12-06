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

import protos from '../../protos';
import {assertFalse, assertTrue} from '../../base/logging';
import {errResult, okResult, Result} from '../../base/result';
import {App} from '../../public/app';
import {RecordSubpage} from './config/config_interfaces';
import {ConfigManager} from './config/config_manager';
import {RecordingTarget} from './interfaces/recording_target';
import {RecordingTargetProvider} from './interfaces/recording_target_provider';
import {
  RECORD_PLUGIN_SCHEMA,
  RECORD_SESSION_SCHEMA,
  RecordPluginSchema,
  RecordSessionSchema,
  SavedSessionSchema,
} from './serialization_schema';
import {TargetPlatformId} from './interfaces/target_platform';
import {TracingSession} from './interfaces/tracing_session';
import {uuidv4} from '../../base/uuid';
import {Time, Timecode} from '../../base/time';

import {getPresetsForPlatform} from './presets';

const LOCALSTORAGE_KEY = 'recordPlugin';

interface LoadConfigOptions {
  config: RecordSessionSchema;
  configId?: string;
  configName?: string;
  configModified?: boolean;
}

export class RecordingManager {
  readonly pages = new Map<string, RecordSubpage>();

  private providers = new Array<RecordingTargetProvider>();
  private platform: TargetPlatformId = 'ANDROID';
  private provider?: RecordingTargetProvider;
  private target?: RecordingTarget;
  private _tracingSession?: CurrentTracingSession;
  recordConfig = new ConfigManager();
  savedConfigs: SavedSessionSchema[] = [];
  selectedConfigId?: string; // ID of currently selected preset or saved config
  selectedConfigName?: string; // Human-readable name of selected config
  private loadedConfigGeneration = 0;
  private initiallyConfigModified = false;
  autoOpenTraceWhenTracingEnds = true;

  constructor(readonly app: App) {}

  registerPage(...pages: RecordSubpage[]) {
    for (const page of pages) {
      assertTrue(!this.pages.has(page.id) || this.pages.get(page.id) === page);
      this.pages.set(page.id, page);
      if (page.kind === 'PROBES_PAGE') {
        this.recordConfig.registerProbes(page.probes);
      }
    }
  }

  registerProvider(provider: RecordingTargetProvider) {
    assertFalse(this.providers.includes(provider));
    this.providers.push(provider);
  }

  get currentPlatform(): TargetPlatformId {
    return this.platform;
  }

  setPlatform(platform: TargetPlatformId) {
    this.platform = platform;
    this.provider = undefined;
    this.target = undefined;
    // If there is only one provider for the platform, auto-select that.
    const filteredProviders = this.listProvidersForCurrentPlatform();
    if (filteredProviders.length === 1) {
      this.provider = filteredProviders[0];
    }
  }

  listProvidersForCurrentPlatform(): RecordingTargetProvider[] {
    return this.providers.filter((p) =>
      p.supportedPlatforms.includes(this.platform),
    );
  }

  get currentProvider(): RecordingTargetProvider | undefined {
    return this.provider;
  }

  getProvider(id: string): RecordingTargetProvider | undefined {
    return this.providers.find((p) => p.id === id);
  }

  async setProvider(provider: RecordingTargetProvider) {
    if (!provider.supportedPlatforms.includes(this.currentPlatform)) {
      // This can happen if the promise that calls refreshTargets() completes
      // after the user has switched to a different platform.
      return;
    }
    this.provider = provider;
    const targets = await provider.listTargets(this.currentPlatform);
    if (this.target && targets.includes(this.target)) {
      return; // The currently selected target is still valid, retain it.
    }
    this.target = targets.length > 0 ? targets[0] : undefined;
    this.app.raf.scheduleFullRedraw();
  }

  async listTargets(): Promise<RecordingTarget[]> {
    if (this.provider === undefined) return [];
    return await this.provider.listTargets(this.currentPlatform);
  }

  get currentSession() {
    return this._tracingSession;
  }

  setTarget(target: RecordingTarget) {
    this.target = target;
  }

  get currentTarget(): RecordingTarget | undefined {
    return this.target;
  }

  genTraceConfig(): protos.TraceConfig {
    return this.recordConfig.genTraceConfig(this.currentPlatform);
  }

  async startTracing(): Promise<CurrentTracingSession> {
    if (this._tracingSession !== undefined) {
      this._tracingSession.session?.cancel();
      this._tracingSession = undefined;
    }
    const traceCfg = this.genTraceConfig();
    const wrappedSession = new CurrentTracingSession(this, traceCfg);
    this._tracingSession = wrappedSession;
    return wrappedSession;
  }

  get isConfigModified() {
    return (
      this.initiallyConfigModified ||
      this.recordConfig.generation !== this.loadedConfigGeneration
    );
  }

  saveConfig(name: string, config: RecordSessionSchema) {
    const existing = this.savedConfigs.find((c) => c.name === name);
    if (existing) {
      existing.config = config;
    } else {
      this.savedConfigs.push({name, config});
    }
    this.persistIntoLocalStorage();
  }

  deleteConfig(name: string) {
    this.savedConfigs = this.savedConfigs.filter((c) => c.name !== name);
    this.persistIntoLocalStorage();
  }

  loadConfig({
    config,
    configId,
    configName,
    configModified = false,
  }: LoadConfigOptions) {
    this.loadSession(config);
    this.selectedConfigId = configId;
    this.selectedConfigName = configName;
    this.loadedConfigGeneration = this.recordConfig.generation;
    this.initiallyConfigModified = configModified;
    this.app.raf.scheduleFullRedraw();
  }

  resolveConfigName(configId: string): string | undefined {
    if (configId.startsWith('preset:')) {
      const presetId = configId.substring(7);
      const presets = getPresetsForPlatform(this.currentPlatform);
      const preset = presets.find((p) => p.id === presetId);
      return preset?.title;
    } else if (configId.startsWith('saved:')) {
      const savedName = configId.substring(6);
      const saved = this.savedConfigs.find((c) => c.name === savedName);
      return saved?.name;
    }
    return undefined;
  }

  clearSelectedConfig() {
    this.selectedConfigId = undefined;
    this.selectedConfigName = undefined;
    this.loadedConfigGeneration = this.recordConfig.generation;
    this.initiallyConfigModified = false;
  }

  loadDefaultConfig() {
    // Load first preset if available
    const presets = getPresetsForPlatform(this.currentPlatform);
    if (presets.length > 0) {
      this.loadConfig({
        config: presets[0].session,
        configId: `preset:${presets[0].id}`,
        configName: presets[0].title,
      });
    } else {
      this.clearSession();
      this.clearSelectedConfig();
    }
  }

  serializeSession(): RecordSessionSchema {
    // Initialize with default values.
    const state: RecordSessionSchema = RECORD_SESSION_SCHEMA.parse({});
    for (const page of this.pages.values()) {
      if (page.kind === 'SESSION_PAGE') {
        page.serialize(state);
      }
    }
    // Serialize the state of each probe page and their settings.
    state.probes = this.recordConfig.serializeProbes();
    return state;
  }

  loadSession(state: RecordSessionSchema): void {
    for (const page of this.pages.values()) {
      if (page.kind === 'SESSION_PAGE') {
        page.deserialize(state);
      }
    }
    this.recordConfig.deserializeProbes(state.probes);
  }

  persistIntoLocalStorage(): void {
    const state: RecordPluginSchema = RECORD_PLUGIN_SCHEMA.parse({});
    state.lastSession = this.serializeSession();
    state.savedSessions = this.savedConfigs;
    for (const page of this.pages.values()) {
      if (page.kind === 'GLOBAL_PAGE') {
        page.serialize(state);
      }
    }
    const json = JSON.stringify(state);
    localStorage.setItem(LOCALSTORAGE_KEY, json);
  }

  restorePluginStateFromLocalstorage(): void {
    const stateJson = localStorage.getItem(LOCALSTORAGE_KEY) ?? '{}';
    let parsedJson: unknown;
    try {
      parsedJson = JSON.parse(stateJson);
    } catch (e) {
      console.error('Record plugin: JSON parse failed', e);
      parsedJson = {};
    }
    const res = RECORD_PLUGIN_SCHEMA.safeParse(parsedJson);
    if (!res.success) {
      throw new Error('Record plugin: deserialization failed', res.error);
    }
    const state = res.data;
    this.savedConfigs = state.savedSessions ?? [];
    for (const page of this.pages.values()) {
      if (page.kind === 'GLOBAL_PAGE') {
        page.deserialize(state);
      }
    }
    // Note: target_selection_page.deserialize() handles loading the session,
    // so we don't need to call loadSession here
  }

  restoreSessionFromJson(json: string): Result<void> {
    let parsedJson: unknown;
    try {
      parsedJson = JSON.parse(json);
    } catch (e) {
      return errResult(`JSON parser error: ${e.message}`);
    }
    const res = RECORD_SESSION_SCHEMA.safeParse(parsedJson);
    if (!res.success) {
      return errResult(`Deserialization error: ${res.error}`);
    }
    this.loadSession(res.data);
    return okResult(undefined);
  }

  clearSession() {
    const emptySession = RECORD_SESSION_SCHEMA.parse({});
    return this.loadSession(emptySession);
  }
}

export class CurrentTracingSession {
  error?: string;
  session?: TracingSession;
  readonly uuid = uuidv4();
  readonly fileName: string;
  readonly isCompressed: boolean;
  private _expectedEndTime: number | undefined;
  private recMgr: RecordingManager;
  private autoOpenedTriggered = false;

  constructor(recMgr: RecordingManager, traceCfg: protos.TraceConfig) {
    this.recMgr = recMgr;
    const now = new Date();
    const ymd = `${now.getFullYear()}${now.getMonth()}${now.getDay()}`;
    const hms = `${now.getHours()}${now.getMinutes()}${now.getSeconds()}`;
    const platLowerCase = recMgr.currentPlatform.toLowerCase();
    this.fileName = `${platLowerCase}-${ymd}-${hms}.pftrace`;
    this.isCompressed = traceCfg.compressionType !== 0;
    if (recMgr.currentTarget === undefined) {
      this.error = 'No target selected';
      return;
    }
    if (recMgr.currentTarget.emitsCompressedtrace) {
      this.fileName += '.gz';
      this.isCompressed = true;
    }
    this.start(traceCfg, recMgr.currentTarget);
  }

  async start(traceCfg: protos.TraceConfig, target: RecordingTarget) {
    const res = await target.startTracing(traceCfg);
    this.recMgr.app.raf.scheduleFullRedraw();
    if (!res.ok) {
      this.error = res.error;
      return;
    }
    const session = (this.session = res.value);

    if (traceCfg.durationMs > 0) {
      this._expectedEndTime = performance.now() + traceCfg.durationMs;
    }

    session.onSessionUpdate.addListener(() => {
      this.recMgr.app.raf.scheduleFullRedraw();
      if (
        session.state === 'FINISHED' &&
        this.recMgr.autoOpenTraceWhenTracingEnds &&
        !this.autoOpenedTriggered
      ) {
        this.autoOpenedTriggered = true;
        this.openTrace();
      }
    });
  }

  get state(): string {
    if (this.error !== undefined) {
      return `Error: ${this.error}`;
    }
    if (this.session === undefined) {
      return 'Initializing';
    }
    return this.session.state;
  }

  get eta(): string | undefined {
    if (this._expectedEndTime === undefined) return undefined;
    let remainingMs = Math.max(this._expectedEndTime - performance.now(), 0);
    if (['FINISHED', 'ERRORED'].includes(this.session?.state ?? '')) {
      remainingMs = 0;
    }
    return new Timecode(Time.fromMillis(remainingMs)).dhhmmss;
  }

  openTrace() {
    const traceData: Uint8Array | undefined = this.session?.getTraceData();
    if (traceData === undefined) return;
    this.recMgr.app.openTraceFromBuffer({
      buffer: traceData,
      title: this.fileName,
      fileName: this.fileName,
    });
  }

  get isCompleted(): boolean {
    return this.session?.state === 'FINISHED';
  }

  get inProgress(): boolean {
    return (
      (this.session === undefined && this.error === undefined) ||
      this.session?.state === 'RECORDING' ||
      this.session?.state === 'STOPPING'
    );
  }
}
