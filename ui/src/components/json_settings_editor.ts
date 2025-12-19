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

import {z} from 'zod';
import m from 'mithril';
import {Editor} from '../widgets/editor';
import {Callout} from '../widgets/callout';
import {Intent} from '../widgets/common';
import {Button} from '../widgets/button';
import {Setting} from '../public/settings';

export interface JsonSettingsEditorOptions<T> {
  // Zod schema for validation
  schema: z.ZodSchema<T>;
  // Optional validator function for additional business logic validation
  validator?: (data: T) => string | undefined;
}

export class JsonSettingsEditor<T> {
  private textareaValue: string | undefined;
  private originalValue: string | undefined;
  private jsonError: string | undefined = undefined;
  private currentSetting: Setting<T> | undefined;

  constructor(private options: JsonSettingsEditorOptions<T>) {}

  render(setting: Setting<T>): m.Children {
    this.currentSetting = setting;
    this.initializeTextValue();

    return m('div', {className: 'pf-json-settings-editor'}, [
      m('div', {className: 'pf-json-settings-editor__editor-section'}, [
        m(Editor, {
          text: this.textareaValue,
          className: 'pf-json-settings-editor__editor',
          onUpdate: (text: string) => this.handleUpdate(text),
          onSave: () => this.handleSave(),
        }),
        this.jsonError !== undefined &&
          m(
            Callout,
            {
              intent: Intent.Danger,
              className: 'pf-json-settings-editor__error',
            },
            `JSON Error: ${this.jsonError}`,
          ),
        m('div', {className: 'pf-json-settings-editor__actions'}, [
          m(Button, {
            label: 'Save',
            disabled: this.isSaveDisabled(),
            onclick: () => this.handleSave(),
          }),
        ]),
      ]),
    ]);
  }

  private initializeTextValue(): void {
    if (this.textareaValue === undefined && this.currentSetting) {
      const data = this.currentSetting.get();
      this.originalValue = this.stringifyData(data);
      this.textareaValue = this.originalValue;
    }
  }

  private stringifyData(data: T): string {
    return JSON.stringify(data, null, 2);
  }

  private handleUpdate(text: string): void {
    this.textareaValue = text;
    this.validateAndSetError(text);
  }

  private handleSave(): void {
    if (this.textareaValue === undefined || !this.currentSetting) return;
    const validatedData = this.validateAndSetError(this.textareaValue);
    if (validatedData !== undefined) {
      this.currentSetting.set(validatedData);
      this.originalValue = this.textareaValue;
    }
  }

  private hasUnsavedChanges(): boolean {
    return this.textareaValue !== this.originalValue;
  }

  private isSaveDisabled(): boolean {
    return this.jsonError !== undefined || !this.hasUnsavedChanges();
  }

  private validateAndSetError(text: string): T | undefined {
    try {
      const parsed = JSON.parse(text);
      const result = this.options.schema.safeParse(parsed);
      if (!result.success) {
        this.jsonError = result.error.issues
          .map((issue) => {
            const path =
              issue.path.length > 0 ? `at ${issue.path.join('.')}` : '';
            return `${issue.message} ${path}`.trim();
          })
          .join(', ');
        return undefined;
      }

      // Run additional validation if provided
      if (this.options.validator) {
        const validationError = this.options.validator(result.data);
        if (validationError !== undefined) {
          this.jsonError = validationError;
          return undefined;
        }
      }

      this.jsonError = undefined;
      return result.data;
    } catch (err) {
      this.jsonError = err instanceof Error ? err.message : 'Invalid JSON';
      return undefined;
    }
  }
}
