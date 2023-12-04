#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""
Enforce import rules for https://ui.perfetto.dev.
Directory structure encodes ideas about the expected dependency graph
of the code in those directories. Both in a fuzzy sense: we expect code
withing a directory to have high cohesion within the directory and low
coupling (aka fewer imports) outside of the directory - but also
concrete rules:
- "base should not depend on the fronted"
- "plugins should only directly depend on the public API"
- "we should not have circular dependencies"

Without enforcement exceptions to this rule quickly slip in. This
script allows such rules to be enforced at presubmit time.
"""
