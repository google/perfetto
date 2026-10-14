# Copyright (C) 2026 The Android Open Source Project
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
"""The extension and trace processor the operator tests run against.

EXT: $PF_EXT, else the extension the Bigtrace server builds from these
sources. TP: $TP_SHELL, else out/release/trace_processor_shell of the
checkout (TP comparisons are skipped when it does not exist).
"""
import os
import sys

_REPO = os.path.abspath(
    os.path.join(os.path.dirname(__file__), *[os.pardir] * 5))
sys.path.insert(0, os.path.join(_REPO, "python"))

from perfetto.bigtrace.server import build_extension  # pylint: disable=wrong-import-position

EXT = os.environ.get("PF_EXT") or build_extension()
TP = os.environ.get(
    "TP_SHELL", os.path.join(_REPO, "out", "release", "trace_processor_shell"))
