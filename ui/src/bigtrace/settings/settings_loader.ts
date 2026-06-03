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

import m from 'mithril';
import {bigTraceSettingsService} from './bigtrace_settings_service';
import type {BigTraceSettingsStore} from './bigtrace_settings_storage';

export type LoadingPhase = 'idle' | 'exec' | 'metadata';

// Handles the async fetch-and-register flow for backend settings.
// Separated from the store so CRUD logic stays synchronous and testable.
export class SettingsLoader {
  loadingPhase: LoadingPhase = 'idle';
  execConfigLoadError: string | undefined = undefined;
  metadataLoadError: string | undefined = undefined;
  private hasLoaded = false;
  private loadPromise: Promise<void> | null = null;

  constructor(private readonly store: BigTraceSettingsStore) {}

  get isLoading(): boolean {
    return this.loadingPhase !== 'idle';
  }

  get loadError(): string | undefined {
    return this.execConfigLoadError || this.metadataLoadError;
  }

  async loadSettings(force = false): Promise<void> {
    if (this.hasLoaded && !force) return;
    if (this.loadPromise) return this.loadPromise;

    this.loadPromise = this.doLoad();
    try {
      await this.loadPromise;
    } finally {
      this.loadPromise = null;
    }
  }

  async reloadMetadataSettings(): Promise<void> {
    this.loadingPhase = 'metadata';
    this.metadataLoadError = undefined;
    m.redraw();

    this.store.removeByCategory('TRACE_METADATA');

    try {
      const filters = this.store.buildSettingFilters();
      this.store.snapshotMetadataFilters(filters);
      const metadataSettings =
        await bigTraceSettingsService.getMetadataSettings(filters);
      for (const setting of metadataSettings) {
        this.store.register(setting);
      }
    } catch (e) {
      this.metadataLoadError = e instanceof Error ? e.message : String(e);
    } finally {
      this.loadingPhase = 'idle';
      m.redraw();
    }
  }

  private async doLoad(): Promise<void> {
    this.loadingPhase = 'exec';
    this.execConfigLoadError = undefined;
    this.metadataLoadError = undefined;
    this.store.clear();
    m.redraw();

    try {
      const execSettings = await bigTraceSettingsService.getExecutionSettings();
      for (const setting of execSettings) {
        this.store.register(setting);
      }
    } catch (e) {
      this.execConfigLoadError = e instanceof Error ? e.message : String(e);
      this.loadingPhase = 'idle';
      this.hasLoaded = true;
      m.redraw();
      return;
    }

    this.loadingPhase = 'metadata';
    m.redraw();

    try {
      const filters = this.store.buildSettingFilters();
      this.store.snapshotMetadataFilters(filters);
      const metadataSettings =
        await bigTraceSettingsService.getMetadataSettings(filters);
      for (const setting of metadataSettings) {
        this.store.register(setting);
      }
    } catch (e) {
      this.metadataLoadError = e instanceof Error ? e.message : String(e);
    } finally {
      this.loadingPhase = 'idle';
      this.hasLoaded = true;
      m.redraw();
    }
  }
}
