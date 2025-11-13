#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

import sys

from code_format_utils import run_code_formatters
from code_format_clang import ClangFormat
from code_format_gn import GnFormat
from code_format_python import Yapf
from code_format_sql import SqlGlot
from code_format_rust import RustFormat
from code_format_ui import UI_CODE_FORMATTERS

if __name__ == '__main__':
  formatters = [
      ClangFormat(),
      GnFormat(),
      Yapf(),
      SqlGlot(),
      RustFormat(),
  ] + UI_CODE_FORMATTERS
  sys.exit(run_code_formatters(formatters))
