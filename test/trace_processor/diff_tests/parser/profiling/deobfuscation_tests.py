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

from python.generators.diff_tests.testing import Path, DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Deobfuscation(TestSuite):
  # When we cannot infer the package from a mapping
  # ("/system/priv-app/Prebuilt1/Prebuilt1.apk"), we'll fall back to the default
  # package for a process, for perf profiles and heap profiles.
  def test_profile_deobfuscation_default_package(self):
    return DiffTestBlueprint(
        trace=Path('profile_unknown_package.textproto'),
        query="""
        SELECT name, deobfuscated_name
        FROM stack_profile_frame
        ORDER BY 1, 2
        """,
        out=Csv("""
        "name","deobfuscated_name"
        "hwe.a","com.google.classfour.foo"
        "hwe.a","com.google.classtwo.foo"
        "hye.a","com.google.classone.foo"
        "hye.a","com.google.classthree.foo"
        """))

  def test_perf_data_symbols_deobfuscation(self):
    return DiffTestBlueprint(
        trace=DataPath('perf-data-deobfuscated.zip'),
        query="""
        SELECT count() AS cnt
        FROM stack_profile_frame
        WHERE deobfuscated_name IS NOT NULL
        """,
        out=Csv("""
        "cnt"
        839
        """))
