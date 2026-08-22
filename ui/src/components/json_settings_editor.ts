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

import './json_settings_editor.scss';
import type {z} from 'zod';
import m from 'mithril';
import {Editor} from '../widgets/editor';
import {Callout} from '../widgets/callout';
import {Intent} from '../widgets/common';
import {Button} from '../widgets/button';
import type {Setting} from '../public/settings';
import {raf} from '../core/raf_scheduler';
import {getErrorMessage} from '../base/errors';

export interface ValidationResult {
  message: string;
  intent?: Intent;
  icon?: string;
}

export interface JsonSettingsEditorOptions<T> {
  // Zod schema for validation
  schema: z.ZodSchema<T>;
  // Optional validator function for additional business logic validation
  validator?: (data: T) => string | undefined;
  // Optional custom validate button action callback
  onValidate?: (
    data: T,
  ) =>
    | Promise<ValidationResult | string | undefined>
    | ValidationResult
    | string
    | undefined;
}

export class JsonSettingsEditor<T> {
  private textareaValue: string | undefined;
  private originalValue: string | undefined;
  private jsonError: string | undefined = undefined;
  private validationResult: ValidationResult | undefined = undefined;
  private isValidating = false;
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
              icon: 'error',
              intent: Intent.Danger,
              className: 'pf-json-settings-editor__error',
            },
            `JSON Error: ${this.jsonError}`,
          ),
        this.jsonError === undefined &&
          this.validationResult !== undefined &&
          m(
            Callout,
            {
              icon:
                this.validationResult.icon ??
                (this.validationResult.intent === Intent.Success
                  ? 'check_circle'
                  : 'error'),
              intent: this.validationResult.intent ?? Intent.None,
              className: 'pf-json-settings-editor__validation-result',
            },
            this.validationResult.message,
          ),
        m('div', {className: 'pf-json-settings-editor__actions'}, [
          this.options.onValidate !== undefined &&
            m(Button, {
              label: this.isValidating ? 'Validating...' : 'Validate',
              disabled: this.isValidateDisabled(),
              onclick: () => this.handleValidate(),
            }),
          m(Button, {
            label: 'Save',
            disabled: this.isSaveDisabled(),
            onclick: () => this.handleSave(),
          }),
        ]),
      ]),
    ]);
  }

  private async runValidation(
    validatedData: T,
  ): Promise<ValidationResult | undefined> {
    if (!this.options.onValidate) {
      return undefined;
    }

    this.isValidating = true;
    this.validationResult = undefined;
    raf.scheduleFullRedraw();
    try {
      const result = await this.options.onValidate(validatedData);
      if (typeof result === 'string') {
        return {
          message: result,
          intent: Intent.Danger,
          icon: 'error',
        };
      } else if (result !== undefined) {
        return result;
      } else {
        return {
          message: 'Validation passed successfully.',
          intent: Intent.Success,
          icon: 'check_circle',
        };
      }
    } catch (err) {
      return {
        message: `Validation error: ${getErrorMessage(err)}`,
        intent: Intent.Danger,
        icon: 'error',
      };
    } finally {
      this.isValidating = false;
      raf.scheduleFullRedraw();
    }
  }

  private async handleValidate(): Promise<void> {
    if (this.textareaValue === undefined) return;
    const validatedData = this.validateAndSetError(this.textareaValue);
    if (validatedData === undefined) {
      this.validationResult = undefined;
      return;
    }

    this.validationResult = await this.runValidation(validatedData);
    raf.scheduleFullRedraw();
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
    this.validationResult = undefined;
    this.validateAndSetError(text);
  }

  private async handleSave(): Promise<void> {
    if (this.textareaValue === undefined || !this.currentSetting) return;
    const validatedData = this.validateAndSetError(this.textareaValue);
    if (validatedData === undefined) return;

    if (this.options.onValidate) {
      const result = await this.runValidation(validatedData);
      this.validationResult = result;
      raf.scheduleFullRedraw();
      if (result && result.intent === Intent.Danger) {
        return;
      }
    }

    this.currentSetting.set(validatedData);
    this.originalValue = this.textareaValue;
    this.validationResult = undefined;
    raf.scheduleFullRedraw();
  }

  private hasUnsavedChanges(): boolean {
    return this.textareaValue !== this.originalValue;
  }

  private isSaveDisabled(): boolean {
    return (
      !this.hasUnsavedChanges() ||
      this.jsonError !== undefined ||
      this.isValidating ||
      (this.validationResult !== undefined &&
        this.validationResult.intent === Intent.Danger)
    );
  }

  private isValidateDisabled(): boolean {
    return (
      !this.hasUnsavedChanges() ||
      this.jsonError !== undefined ||
      this.isValidating ||
      this.validationResult !== undefined
    );
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
