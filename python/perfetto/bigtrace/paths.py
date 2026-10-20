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
"""Where the local Bigtrace server keeps its files (XDG base directories)."""

import os


def _xdg(var, default):
  value = os.environ.get(var, "")
  # The XDG spec says relative paths are invalid and must be ignored.
  return value if os.path.isabs(value) else os.path.expanduser(default)


# Built DuckDB extensions, downloaded DuckDB headers and unpacked archives.
CACHE_DIR = os.path.join(
    _xdg("XDG_CACHE_HOME", "~/.cache"), "perfetto", "bigtrace")
# The `install`ed service: its settings and its systemd user unit.
CONFIG_FILE = os.path.join(
    _xdg("XDG_CONFIG_HOME", "~/.config"), "perfetto", "bigtrace.json")
UNIT_DIR = os.path.join(_xdg("XDG_CONFIG_HOME", "~/.config"), "systemd", "user")
# The service's DuckDB database: result tables and query history.
DATA_DIR = os.path.join(
    _xdg("XDG_DATA_HOME", "~/.local/share"), "perfetto", "bigtrace")
