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

/**
 * Manages feature flags for experimental or togglable features.
 *
 * Feature flags allow plugins to expose experimental functionality that
 * users can enable/disable. Flags are persisted across sessions and can be
 * configured on the flags page.
 */
export interface FeatureFlagManager {
  /**
   * Registers a new feature flag.
   *
   * @param settings The settings for the new feature flag.
   * @returns The registered feature flag.
   */
  register(settings: FlagSettings): Flag;
}

/**
 * Settings for defining a new feature flag.
 */
export interface FlagSettings {
  /**
   * A unique identifier for this flag (e.g., "magicSorting").
   */
  readonly id: string;

  /**
   * The default value of the flag (true or false).
   *
   * If `flag.isOverridden()` is false, then `flag.get()` will return
   * `flag.defaultValue`.
   */
  readonly defaultValue: boolean;

  /**
   * A longer description which is displayed to the user.
   *
   * Example: "Sort tracks using an embedded tfLite model based on your
   * expression while waiting for the trace to load."
   */
  readonly description: string;

  /**
   * The name of the flag the user sees (e.g., "New track sorting algorithm").
   * If omitted, the `id` will be used as the name.
   */
  readonly name?: string;

  /**
   * If true, this flag will only be visible and configurable in development
   * builds of the Perfetto UI.
   */
  readonly devOnly?: boolean;
}

/**
 * Represents a feature flag that can be enabled or disabled.
 */
export interface Flag {
  /**
   * A unique identifier for this flag (e.g., "magicSorting").
   */
  readonly id: string;

  /**
   * The name of the flag the user sees (e.g., "New track sorting algorithm").
   */
  readonly name: string;

  /**
   * A longer description which is displayed to the user.
   *
   * Example: "Sort tracks using an embedded tfLite model based on your
   * expression while waiting for the trace to load."
   */
  readonly description: string;

  /**
   * Whether the flag defaults to true or false.
   *
   * If `!flag.isOverridden()`, then `flag.get()` will return
   * `flag.defaultValue`.
   */
  readonly defaultValue: boolean;

  /**
   * Get the current value of the flag.
   *
   * @returns The current boolean value of the flag.
   */
  get(): boolean;

  /**
   * Override the flag and persist the new value.
   *
   * This will change the flag's value for the current and future sessions.
   *
   * @param value The new boolean value for the flag.
   */
  set(value: boolean): void;

  /**
   * Checks if the flag has been explicitly overridden by the user.
   *
   * Note: A flag can be overridden to its default value.
   *
   * @returns `true` if the flag's value has been explicitly set, `false`
   *   otherwise.
   */
  isOverridden(): boolean;

  /**
   * Reset the flag to its default setting.
   *
   * This will remove any user override and revert the flag to its
   * `defaultValue`.
   */
  reset(): void;

  /**
   * Get the current state of the flag's override status.
   *
   * @returns The {@link OverrideState} of the flag.
   */
  overriddenState(): OverrideState;
}

/**
 * Represents the override state of a feature flag.
 */
export enum OverrideState {
  /** The flag is currently using its default value. */
  DEFAULT = 'DEFAULT',
  /** The flag has been overridden to `true`. */
  TRUE = 'OVERRIDE_TRUE',
  /** The flag has been overridden to `false`. */
  FALSE = 'OVERRIDE_FALSE',
}
