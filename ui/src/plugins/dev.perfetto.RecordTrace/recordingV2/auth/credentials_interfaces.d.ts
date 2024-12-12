// Copyright (C) 2022 The Android Open Source Project
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

// Typescript interfaces for PasswordCredential don't exist as of
// lib.dom es2020 (see tsconfig.json), so we had to define them here.
declare global {
  export interface PasswordCredentialData {
    readonly id: string;
    readonly name: string;
    readonly iconURL: string;
    readonly password: string;
  }

  export class PasswordCredential extends Credential {
    password: string;
    constructor(data: PasswordCredentialData);
  }

  export interface PasswordCredentialRequestOptions
    extends CredentialRequestOptions {
    password?: boolean;
  }
}

// we can only augment the global scope from an external module
export {};
