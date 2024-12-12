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

export interface FeatureFlagManager {
  register(settings: FlagSettings): Flag;
}

export interface FlagSettings {
  id: string;
  defaultValue: boolean;
  description: string;
  name?: string;
  devOnly?: boolean;
}

export interface Flag {
  // A unique identifier for this flag ("magicSorting")
  readonly id: string;

  // The name of the flag the user sees ("New track sorting algorithm")
  readonly name: string;

  // A longer description which is displayed to the user.
  // "Sort tracks using an embedded tfLite model based on your expression
  // while waiting for the trace to load."
  readonly description: string;

  // Whether the flag defaults to true or false.
  // If !flag.isOverridden() then flag.get() === flag.defaultValue
  readonly defaultValue: boolean;

  // Get the current value of the flag.
  get(): boolean;

  // Override the flag and persist the new value.
  set(value: boolean): void;

  // If the flag has been overridden.
  // Note: A flag can be overridden to its default value.
  isOverridden(): boolean;

  // Reset the flag to its default setting.
  reset(): void;

  // Get the current state of the flag.
  overriddenState(): OverrideState;
}

export enum OverrideState {
  DEFAULT = 'DEFAULT',
  TRUE = 'OVERRIDE_TRUE',
  FALSE = 'OVERRIDE_FALSE',
}
