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
import {assertFalse} from '../../base/logging';
import {App} from '../../public/app';
import {RecordingTarget} from './interfaces/recording_target';
import {RecordingTargetProvider} from './interfaces/recording_target_provider';

import {TargetPlatformId} from './interfaces/target_platform';
import {TracingSession} from './interfaces/tracing_session';
import {uuidv4} from '../../base/uuid';
import {Time, Timecode} from '../../base/time';

export class RecordingManager {
  private providers = new Array<RecordingTargetProvider>();
  private platform: TargetPlatformId = 'ANDROID';
  private provider?: RecordingTargetProvider;
  private target?: RecordingTarget;
  private _tracingSession?: CurrentTracingSession;
  autoOpenTraceWhenTracingEnds = true;

  constructor(readonly app: App) {}

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
    this.app.scheduleFullRedraw('force');
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
    this.recMgr.app.scheduleFullRedraw('force');
    if (!res.ok) {
      this.error = res.error;
      return;
    }
    const session = (this.session = res.value);

    if (traceCfg.durationMs > 0) {
      this._expectedEndTime = performance.now() + traceCfg.durationMs;
    }

    session.onSessionUpdate.addListener(() => {
      this.recMgr.app.scheduleFullRedraw('force');
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
