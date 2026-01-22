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

import {
  LanguageModelManager,
  LanguageModelProvider,
} from '../public/language_model';
import {Setting} from '../public/settings';

/**
 * Implementation of the LanguageModelManager that plugins can use to
 * register language model providers.
 */
export class LanguageModelManagerImpl implements LanguageModelManager {
  private providers = new Map<string, LanguageModelProvider>();
  private preferredProviderSetting?: Setting<string>;

  /**
   * Set the setting to use for storing the preferred provider.
   * This is called by AppImpl after the setting is registered.
   */
  setPreferredProviderSetting(setting: Setting<string>): void {
    this.preferredProviderSetting = setting;
  }

  registerProvider(provider: LanguageModelProvider): void {
    this.providers.set(provider.info.id, provider);
  }

  getProviders(): LanguageModelProvider[] {
    return Array.from(this.providers.values());
  }

  getProvider(id: string): LanguageModelProvider | undefined {
    return this.providers.get(id);
  }

  getPreferredProviderId(): string {
    return this.preferredProviderSetting?.get() ?? 'gemini-nano';
  }

  setPreferredProviderId(id: string): void {
    this.preferredProviderSetting?.set(id);
  }

  getPreferredProvider(): LanguageModelProvider | undefined {
    const preferred = this.providers.get(this.getPreferredProviderId());
    if (preferred) return preferred;

    // Fall back to first available provider
    const providers = this.getProviders();
    return providers.length > 0 ? providers[0] : undefined;
  }
}
