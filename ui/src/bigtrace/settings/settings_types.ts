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

export type SettingCategory =
  | 'SETTING_CATEGORY_UNSPECIFIED'
  | 'TRACE_ADDRESS'
  | 'TRACE_METADATA'
  | 'BIGTRACE_QUERY_OPTIONS';

export interface EnumOption {
  readonly value: string;
  readonly label: string;
}

export interface SettingDescriptor<T> {
  // A unique identifier for the setting.
  readonly id: string;

  // A human-readable name for the setting.
  readonly name: string;

  // A detailed description of what the setting does.
  readonly description: string;

  // The type of the setting.
  readonly type:
    | 'string'
    | 'number'
    | 'boolean'
    | 'enum'
    | 'multi-select'
    | 'string-array';

  // The Zod schema for validating the setting's value.
  readonly schema: z.ZodType<T>;

  // The default value of the setting.
  readonly defaultValue: T;

  // The category for grouping the setting in the UI.
  readonly category?: string;

  // If true, the user will be prompted to reload when this setting is changed.
  readonly requiresReload?: boolean;

  // Optional list of choices for enum and multi-select settings.
  readonly options?: readonly (string | EnumOption)[];

  // Optional placeholder for text inputs.
  readonly placeholder?: string;

  // Optional format for string inputs.
  readonly format?: 'sql';

  // If true, this setting will be disabled by default.
  // This is designed to be set by the server.
  readonly disabled?: boolean;
}

export interface Setting<T> extends SettingDescriptor<T> {
  readonly isDefault: boolean;
  get(): T;
  set(value: T): void;
  reset(): void;
  isDisabled(): boolean;
  setDisabled(disabled: boolean): void;
  [Symbol.dispose](): void;
}
export interface SettingFilter {
  readonly settingId: string;
  readonly values: string[];
  readonly category: SettingCategory;
}
