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

// Imported before any module that defines zod schemas (settings register and
// parse their values when imported).

import {z} from 'zod';

// The Bigtrace UI's CSP has no 'unsafe-eval' (and deployments may add their
// own, stricter header): keep zod from compiling object schemas with `new
// Function`, which the CSP blocks and reports as a violation.
z.config({jitless: true});
