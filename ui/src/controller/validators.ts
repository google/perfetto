// Copyright (C) 2021 The Android Open Source Project
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

// Execution context of object validator
interface ValidatorContext {
  // Path to the current value starting from the root. Object field names are
  // stored as is, array indices are wrapped to square brackets. Represented
  // as an array to avoid unnecessary string concatenation: parts are going to
  // be concatenated into a single string when reporting errors, which should
  // not happen on a happy path.
  // Example: ["config", "androidLogBuffers", "1"] when parsing object
  // accessible through expression `root.config.androidLogBuffers[1]`
  path: string[];

  // Paths from the root to extraneous keys in a validated object.
  extraKeys: string[];

  // Paths from the root to keys containing values of wrong type in validated
  // object.
  invalidKeys: string[];
}

// Validator accepting arbitrary data structure and returning a typed value.
// Can throw an error if a part of the value does not have a reasonable
// default.
export interface Validator<T> {
  validate(input: unknown, context: ValidatorContext): T;
}

// Helper function to flatten array of path chunks into a single string
// Example: ["config", "androidLogBuffers", "1"] is mapped to
// "config.androidLogBuffers[1]".
function renderPath(path: string[]): string {
  let result = '';
  for (let i = 0; i < path.length; i++) {
    if (i > 0 && !path[i].startsWith('[')) {
      result += '.';
    }
    result += path[i];
  }
  return result;
}

export class ValidationError extends Error {}

// Abstract class for validating simple values, such as strings and booleans.
// Allows to avoid repetition of most of the code related to validation of
// these.
abstract class PrimitiveValidator<T> implements Validator<T> {
  defaultValue: T;
  required: boolean;

  constructor(defaultValue: T, required: boolean) {
    this.defaultValue = defaultValue;
    this.required = required;
  }

  // Abstract method that checks whether passed input has correct type.
  abstract predicate(input: unknown): input is T;

  validate(input: unknown, context: ValidatorContext): T {
    if (this.predicate(input)) {
      return input;
    }
    if (this.required) {
      throw new ValidationError(renderPath(context.path));
    }
    if (input !== undefined) {
      // The value is defined, but does not conform to the expected type;
      // proceed with returning the default value but report the key.
      context.invalidKeys.push(renderPath(context.path));
    }
    return this.defaultValue;
  }
}


class StringValidator extends PrimitiveValidator<string> {
  predicate(input: unknown): input is string {
    return typeof input === 'string';
  }
}

class NumberValidator extends PrimitiveValidator<number> {
  predicate(input: unknown): input is number {
    return typeof input === 'number';
  }
}

class BooleanValidator extends PrimitiveValidator<boolean> {
  predicate(input: unknown): input is boolean {
    return typeof input === 'boolean';
  }
}

// Type-level function returning resulting type of a validator.
export type ValidatedType<T> = T extends Validator<infer S>? S : never;

// Type-level function traversing a record of validator and returning record
// with the same keys and valid types.
export type RecordValidatedType<T> = {
  [k in keyof T]: ValidatedType<T[k]>
};

// Combinator for validators: takes a record of validators, and returns a
// validator for a record where record's fields passed to validator with the
// same name.
//
// Generic parameter T is instantiated to type of record of validators, and
// should be provided implicitly by type inference due to verbosity of its
// instantiations.
class RecordValidator<T extends Record<string, Validator<unknown>>> implements
    Validator<RecordValidatedType<T>> {
  validators: T;

  constructor(validators: T) {
    this.validators = validators;
  }

  validate(input: unknown, context: ValidatorContext): RecordValidatedType<T> {
    // If value is missing or of incorrect type, empty record is still processed
    // in the loop below to initialize default fields of the nested object.
    let o: object = {};
    if (typeof input === 'object' && input !== null) {
      o = input;
    } else if (input !== undefined) {
      context.invalidKeys.push(renderPath(context.path));
    }

    const result: Partial<RecordValidatedType<T>> = {};
    // Separate declaration is required to avoid assigning `string` type to `k`.
    for (const k in this.validators) {
      if (this.validators.hasOwnProperty(k)) {
        context.path.push(k);
        const validator = this.validators[k];

        // Accessing value of `k` of `o` is safe because `undefined` values are
        // considered to indicate a missing value and handled appropriately by
        // every provided validator.
        const valid =
            validator.validate((o as Record<string, unknown>)[k], context);

        result[k] = valid as ValidatedType<T[string]>;
        context.path.pop();
      }
    }

    // Check if passed object has any extra keys to be reported as such.
    for (const key of Object.keys(o)) {
      if (!this.validators.hasOwnProperty(key)) {
        context.path.push(key);
        context.extraKeys.push(renderPath(context.path));
        context.path.pop();
      }
    }
    return result as RecordValidatedType<T>;
  }
}

// Validator checking whether a value is one of preset values. Used in order to
// provide easy validation for union of literal types.
class OneOfValidator<T> implements Validator<T> {
  validValues: readonly T[];
  defaultValue: T;

  constructor(validValues: readonly T[], defaultValue: T) {
    this.defaultValue = defaultValue;
    this.validValues = validValues;
  }

  validate(input: unknown, context: ValidatorContext): T {
    if (this.validValues.includes(input as T)) {
      return input as T;
    } else if (input !== undefined) {
      context.invalidKeys.push(renderPath(context.path));
    }
    return this.defaultValue;
  }
}

// Validator for an array of elements, applying the same element validator for
// each element of an array. Uses empty array as a default value.
class ArrayValidator<T> implements Validator<T[]> {
  elementValidator: Validator<T>;

  constructor(elementValidator: Validator<T>) {
    this.elementValidator = elementValidator;
  }

  validate(input: unknown, context: ValidatorContext): T[] {
    const result: T[] = [];
    if (Array.isArray(input)) {
      for (let i = 0; i < input.length; i++) {
        context.path.push(`[${i}]`);
        result.push(this.elementValidator.validate(input[i], context));
        context.path.pop();
      }
    } else if (input !== undefined) {
      context.invalidKeys.push(renderPath(context.path));
    }
    return result;
  }
}

// Wrapper container for validation result contaiting diagnostic information in
// addition to the resulting typed value.
export interface ValidationResult<T> {
  result: T;
  invalidKeys: string[];
  extraKeys: string[];
}

// Wrapper for running a validator initializing the context.
export function runValidator<T>(
    validator: Validator<T>, input: unknown): ValidationResult<T> {
  const context: ValidatorContext = {
    path: [],
    invalidKeys: [],
    extraKeys: [],
  };
  const result = validator.validate(input, context);
  return {
    result,
    invalidKeys: context.invalidKeys,
    extraKeys: context.extraKeys,
  };
}

// Shorthands for the validator classes above enabling concise notation.
export function str(defaultValue = ''): StringValidator {
  return new StringValidator(defaultValue, false);
}

export const requiredStr = new StringValidator('', true);

export function num(defaultValue = 0): NumberValidator {
  return new NumberValidator(defaultValue, false);
}

export function bool(defaultValue = false): BooleanValidator {
  return new BooleanValidator(defaultValue, false);
}

export function record<T extends Record<string, Validator<unknown>>>(
    validators: T): RecordValidator<T> {
  return new RecordValidator<T>(validators);
}

export function oneOf<T>(
    values: readonly T[], defaultValue: T): OneOfValidator<T> {
  return new OneOfValidator<T>(values, defaultValue);
}

export function arrayOf<T>(elementValidator: Validator<T>): ArrayValidator<T> {
  return new ArrayValidator<T>(elementValidator);
}
