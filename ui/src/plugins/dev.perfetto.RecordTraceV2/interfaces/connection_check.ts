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

import m from 'mithril';
import {Result} from '../../../base/result';

export interface WithPreflightChecks {
  /**
   * yields a sequence of diagnostic check that should be performed before
   * starting the connection. Those checks provide actionable information about
   * missed preconditions.
   */
  runPreflightChecks(): AsyncGenerator<PreflightCheck>;
}

export type PreflightCheckResult = Result<string>;

export interface PreflightCheck {
  /** E.g. "Check Android Version", "Check WebUSB Connection" */
  readonly name: string;

  /**
   * 1. An OK status, if the check succeeds. In this case the value can carry a
   *    message (e.g. "Connected, version 1.2.3").
   * 2. An Error status, alongside the message: (e.g. "Could not connect to
   *    127.0.0.1:1234").
   */
  readonly status: PreflightCheckResult;

  /**
   * [Optional] A mithril component that shows instruction on how to remediate
   * to the problem. For cases where the StatusOr error is not enough and we
   * want to show something more interactive.
   */
  readonly remediation?: m.ComponentTypes;
}
