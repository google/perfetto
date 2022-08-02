#!/usr/bin/env python3
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

# This file should do the same thing when being invoked in any of these ways:
# ./trace_processor
# python trace_processor
# bash trace_processor
# cat ./trace_processor | bash
# cat ./trace_processor | python -

BASH_FALLBACK=""" "
exec python3 - "$@" <<'#'EOF
#"""  # yapf: disable

from perfetto.prebuilts.manifests.trace_processor_shell import *
from perfetto.prebuilts.perfetto_prebuilts import *

if __name__ == '__main__':
  run_perfetto_prebuilt(TRACE_PROCESSOR_SHELL_MANIFEST)

#EOF
