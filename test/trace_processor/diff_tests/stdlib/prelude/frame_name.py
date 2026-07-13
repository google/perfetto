#!/usr/bin/env python3
#
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class FrameName(TestSuite):

  def test_frame_name(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        SELECT
          -- A symbol name is preferred and demangled.
          __intrinsic_frame_name('_Z3foov', NULL, 'raw', NULL, 100,
            '/a/b/libx.so') AS symbol,
          -- A non-mangled name is kept as-is.
          __intrinsic_frame_name(NULL, NULL, 'my_func', NULL, 100,
            '/a/b/libx.so') AS raw_name,
          -- The deobfuscated name wins over an empty frame name.
          __intrinsic_frame_name(NULL, 'com.Foo.bar', '', NULL, 100,
            '/a/b/libx.so') AS deobfuscated,
          -- Unsymbolized native frame: address plus the mapping basename.
          __intrinsic_frame_name(NULL, NULL, '', NULL, 6699,
            '/apex/x/lib64/libart.so') AS unsym_mapping,
          -- Unsymbolized native frame with no mapping name: just the address.
          __intrinsic_frame_name(NULL, NULL, '', NULL, 4660, '') AS unsym_empty,
          __intrinsic_frame_name(NULL, NULL, NULL, NULL, 4660, NULL)
            AS unsym_null,
          -- No name but source info present (e.g. anonymous JS): kept empty.
          __intrinsic_frame_name(NULL, NULL, '', 'http://x/y.js', 0, NULL)
            AS anonymous
        """,
        out=Csv("""
        "symbol","raw_name","deobfuscated","unsym_mapping","unsym_empty","unsym_null","anonymous"
        "foo()","my_func","com.Foo.bar","0x1a2b @ libart.so","0x1234","0x1234",""
        """))
