// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

export class NodeIssues {
  queryError?: Error;
  responseError?: Error;
  dataError?: Error;
  warnings: Error[] = [];

  hasIssues(): boolean {
    return (
      this.queryError !== undefined ||
      this.responseError !== undefined ||
      this.dataError !== undefined ||
      this.warnings.length > 0
    );
  }

  getTitle(): string {
    let title = '';
    if (this.queryError) {
      title += `Query Error: ${this.queryError.message}\n`;
    }
    if (this.responseError) {
      title += `Response Error: ${this.responseError.message}\n`;
    }
    if (this.dataError) {
      title += `Data Error: ${this.dataError.message}\n`;
    }
    if (this.warnings.length > 0) {
      title += `Warnings:\n${this.warnings.join('\n')}`;
    }
    return title;
  }

  clear() {
    this.queryError = undefined;
    this.responseError = undefined;
    this.dataError = undefined;
    this.warnings = [];
  }
}

/**
 * Helper function to set a validation error on a node's state.
 * Creates a NodeIssues instance if one doesn't exist and sets the queryError.
 *
 * @param state - The node state object that may contain issues
 * @param state.issues - Optional NodeIssues instance
 * @param message - The error message to set
 */
export function setValidationError(
  state: {issues?: NodeIssues},
  message: string,
): void {
  if (!state.issues) {
    state.issues = new NodeIssues();
  }
  state.issues.queryError = new Error(message);
}
