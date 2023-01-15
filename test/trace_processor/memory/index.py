#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Memory(DiffTestModule):

  def test_android_mem_counters(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Metric('android_mem'),
        out=Path('android_mem_counters.out'))

  def test_trace_metadata(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Metric('trace_metadata'),
        out=Path('trace_metadata.out'))

  def test_android_mem_by_priority(self):
    return DiffTestBlueprint(
        trace=Path('android_mem_by_priority.py'),
        query=Metric('android_mem'),
        out=Path('android_mem_by_priority.out'))

  def test_android_mem_lmk(self):
    return DiffTestBlueprint(
        trace=Path('android_systrace_lmk.py'),
        query=Metric('android_lmk'),
        out=Path('android_mem_lmk.out'))

  def test_android_lmk_oom(self):
    return DiffTestBlueprint(
        trace=Path('../common/oom_kill.textproto'),
        query=Metric('android_lmk'),
        out=Path('android_lmk_oom.out'))

  def test_android_mem_delta(self):
    return DiffTestBlueprint(
        trace=Path('android_mem_delta.py'),
        query=Metric('android_mem'),
        out=Path('android_mem_delta.out'))

  def test_android_ion(self):
    return DiffTestBlueprint(
        trace=Path('android_ion.py'),
        query=Metric('android_ion'),
        out=Path('android_ion.out'))

  def test_android_ion_stat(self):
    return DiffTestBlueprint(
        trace=Path('android_ion_stat.textproto'),
        query=Metric('android_ion'),
        out=Path('android_ion_stat.out'))

  def test_android_dma_heap_stat(self):
    return DiffTestBlueprint(
        trace=Path('android_dma_heap_stat.textproto'),
        query=Metric('android_dma_heap'),
        out=Path('android_dma_heap_stat.out'))

  def test_android_dma_buffer_tracks(self):
    return DiffTestBlueprint(
        trace=Path('android_dma_heap_stat.textproto'),
        query=Path('dma_buffer_tracks_test.sql'),
        out=Path('android_dma_buffer_tracks.out'))

  def test_android_fastrpc_dma_stat(self):
    return DiffTestBlueprint(
        trace=Path('android_fastrpc_dma_stat.textproto'),
        query=Metric('android_fastrpc'),
        out=Path('android_fastrpc_dma_stat.out'))

  def test_shrink_slab(self):
    return DiffTestBlueprint(
        trace=Path('shrink_slab.textproto'),
        query=Path('shrink_slab_test.sql'),
        out=Path('shrink_slab.out'))

  def test_cma(self):
    return DiffTestBlueprint(
        trace=Path('cma.textproto'),
        query=Path('cma_test.sql'),
        out=Path('cma.out'))
