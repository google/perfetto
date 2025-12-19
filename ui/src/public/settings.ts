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

/**
 * Defines the core interfaces and types for managing application settings.
 * This allows different parts of the application to register settings,
 * retrieve their values, and potentially render custom UI for them.
 */

import {z} from 'zod';
import m from 'mithril';

export type SettingRenderer<T> = (setting: Setting<T>) => m.Children;

/**
 * Describes a setting before it is registered with the SettingsManager.
 * This interface defines the static properties of a setting.
 * @template T The type of the setting's value.
 */
export interface SettingDescriptor<T> {
  // A unique identifier for the setting. Used as the storage key.
  readonly id: string;
  // A human-readable name for the setting, used on the settings page.
  readonly name: string;
  // A detailed description of what the setting does, used on the settings page.
  readonly description: string;
  // The Zod schema used for validating the setting's value, and defining the
  // structure and type of this setting.
  readonly schema: z.ZodType<T>;
  // The default value of the setting if the setting is absent from the
  // underlying storage.
  readonly defaultValue: T;
  // If true, the user will be prompted to reload the the page when this setting
  // is changed.
  readonly requiresReload?: boolean;
  // An optional render function for customizing the UI of this setting in the
  // settings page. Required for settings that are move complex than a primitive
  // type, such as objects or arrays.
  readonly render?: SettingRenderer<T>;
}

/**
 * Represents a registered setting instance.
 * It includes all properties from the descriptor, plus methods to interact
 * with the setting's value and state.
 * @template T The type of the setting's value.
 */
export interface Setting<T> extends SettingDescriptor<T>, Disposable {
  // Returns true if this settings is currently set to the default value.
  readonly isDefault: boolean;
  // Get the current value of the setting.
  get(): T;
  // Set the value of the setting. This will also update the underlying storage.
  set(value: T): void;
  // Resets back to default.
  reset(): void;
}

/**
 * Manages the registration and retrieval of application settings.
 */
export interface SettingsManager {
  /**
   * Registers a new setting.
   * @returns A handle used to interact with the setting.
   */
  register<T>(setting: SettingDescriptor<T>): Setting<T>;
  /**
   * Resets all settings back to their default values.
   */
  resetAll(): void;
  /**
   * Retrieves a list of all currently registered settings.
   */
  getAllSettings(): ReadonlyArray<Setting<unknown>>;
  /**
   * Checks if any setting that requires a reload has been modified from its
   * value at the time of the last reload/initial load.
   * @returns True if a reload is required, false otherwise.
   */
  isReloadRequired(): boolean;

  /**
   * Get the a setting by its ID.
   * @param id The unique identifier of the setting.
   */
  get<T>(id: string): Setting<T> | undefined;
}
