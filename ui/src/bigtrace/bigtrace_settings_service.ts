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

import { z } from 'zod';
import { SettingDescriptor, SettingCategory, SettingFilter } from './settings_types';
import { endpointManager } from './endpoint_manager';

interface BackendSettingOption {
  value?: string;
  label?: string;
}

interface BackendSetting {
  id?: string;
  name?: string;
  description?: string;
  disabled?: boolean;
  category?: SettingCategory;

  number?: { defaultValue?: number; min?: number; max?: number };
  stringEnum?: { defaultValue?: string; options?: BackendSettingOption[] };
  multiSelect?: { defaultValues?: string[]; options?: BackendSettingOption[] };
  plainString?: { defaultValue?: string };
  stringArray?: { defaultValues?: string[] };
  booleanOptions?: { defaultValue?: boolean };
}

function toSettingDescriptor(option: BackendSetting): SettingDescriptor<unknown> {
  const { id = '', name = '', description = '', disabled, category } = option;

  let type: 'string' | 'number' | 'boolean' | 'enum' | 'multi-select' | 'string-array' = 'string';
  let schema: z.ZodType<any> = z.any();
  let defaultValue: any = undefined;
  let optionsList: { value: string, label: string }[] | undefined = undefined;

  if (option.number) {
    type = 'number';
    schema = z.number();
    if (option.number.min !== undefined && option.number.min !== 0) schema = (schema as z.ZodNumber).min(option.number.min);
    if (option.number.max !== undefined && option.number.max !== 0) schema = (schema as z.ZodNumber).max(option.number.max);
    defaultValue = option.number.defaultValue ?? 0;
  } else if (option.stringEnum) {
    type = 'enum';
    const enumOptions = (option.stringEnum.options || []).map(o => o.value || '');
    schema = enumOptions.length > 0 ? z.enum(enumOptions as [string, ...string[]]) : z.string();
    defaultValue = option.stringEnum.defaultValue ?? '';
    optionsList = (option.stringEnum.options || []).map(o => ({ value: o.value || '', label: o.label || o.value || '' }));
  } else if (option.multiSelect) {
    type = 'multi-select';
    schema = z.array(z.string());
    defaultValue = option.multiSelect.defaultValues || [];
    optionsList = (option.multiSelect.options || []).map(o => ({ value: o.value || '', label: o.label || o.value || '' }));
  } else if (option.plainString) {
    type = 'string';
    schema = z.string();
    defaultValue = option.plainString.defaultValue || '';
  } else if (option.stringArray) {
    type = 'string-array';
    schema = z.array(z.string());
    defaultValue = option.stringArray.defaultValues || [];
  } else if (option.booleanOptions) {
    type = 'boolean';
    schema = z.boolean();
    defaultValue = option.booleanOptions.defaultValue ?? false;
  }

  return {
    id,
    name,
    description,
    type,
    schema,
    defaultValue,
    category,
    options: optionsList,
    disabled: disabled ?? false,
  };
}

class BigTraceSettingsService {
  async getExecutionSettings(): Promise<SettingDescriptor<unknown>[]> {
    const endpointSetting = endpointManager.get('bigtraceEndpoint');
    const endpoint = endpointSetting ? endpointSetting.get() as string : '';

    let options: BackendSetting[] = [];

    try {
      const response = await fetch(`${endpoint}/v1/bigtrace_execution_config`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: '{}',
        credentials: 'include',
        mode: 'cors',
      });

      if (response.ok) {
        const text = await response.text();
        try {
          const data = JSON.parse(text);
          options = data.setting || [];
        } catch (e) {
          console.error('Failed to parse bigtrace_execution_config response:', text.substring(0, 100));
          throw new Error('Failed to parse bigtrace_execution_config response');
        }
      } else {
        console.error(`bigtrace_execution_config failed with status ${response.status}`);
        throw new Error(`Execution config failed: HTTP ${response.status}`);
      }
    } catch (err) {
      console.error('Error fetching bigtrace_execution_config:', err);
      if (err instanceof TypeError) {
        throw new Error('Cannot connect to the BigTrace backend. Please check your endpoint address and network connection.');
      }
      throw err;
    }

    return options.map(toSettingDescriptor);
  }

  async getMetadataSettings(filters: SettingFilter[]): Promise<SettingDescriptor<unknown>[]> {
    const endpointSetting = endpointManager.get('bigtraceEndpoint');
    const endpoint = endpointSetting ? endpointSetting.get() as string : '';

    let options: BackendSetting[] = [];

    try {
      const response = await fetch(`${endpoint}/v1/trace_metadata_settings`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ settings: filters }),
        credentials: 'include',
        mode: 'cors',
      });

      if (response.ok) {
        const text = await response.text();
        try {
          const data = JSON.parse(text);
          options = data.setting || [];
        } catch (e) {
          console.error('Failed to parse trace_metadata_settings response:', text.substring(0, 100));
          throw new Error('Failed to parse trace_metadata_settings response');
        }
      } else {
        console.error(`trace_metadata_settings failed with status ${response.status}`);
        throw new Error(`Trace metadata config failed: HTTP ${response.status}`);
      }
    } catch (err) {
      console.error('Error fetching trace_metadata_settings:', err);
      if (err instanceof TypeError) {
        throw new Error('Cannot connect to the BigTrace backend. Please check your endpoint address and network connection.');
      }
      throw err;
    }

    return options.map(toSettingDescriptor);
  }
}

export const bigTraceSettingsService = new BigTraceSettingsService();
