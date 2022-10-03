# Copyright (C) 2021 The Android Open Source Project
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

import os


def repo_root():
  """ Finds the repo root by traversing up the hierarchy

  This is for use in scripts that get amalgamated, where _file_ can be either
  python/perfetto/... or tools/amalgamated_tool.
  """
  path = os.path.dirname(os.path.abspath(__file__))  # amalgamator:nocheck
  last_dir = ''
  while path and path != last_dir:
    if os.path.exists(os.path.join(path, 'perfetto.rc')):
      return path
    last_dir = path
    path = os.path.dirname(path)
  return None


def repo_dir(rel_path):
  return os.path.join(repo_root() or '', rel_path)
