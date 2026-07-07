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

import type {z} from 'zod';

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
  readonly id: string;

  readonly name: string;

  readonly description: string;

  // Defaults to 'string' if omitted.
  readonly type?:
    | 'string'
    | 'number'
    | 'boolean'
    | 'enum'
    | 'multi-select'
    | 'string-array';

  // Zod schema validating the value.
  readonly schema: z.ZodType<T>;

  readonly defaultValue: T;

  // Groups the setting in the UI.
  readonly category?: string;

  // If true, prompt the user to reload on change.
  readonly requiresReload?: boolean;

  // Choices for enum and multi-select settings.
  readonly options?: readonly (string | EnumOption)[];

  readonly placeholder?: string;

  readonly format?: 'sql';

  // Disabled by default; set by the server.
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
