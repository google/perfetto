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
"""Tests for serving zip and tar archives of traces."""

import io
import os
import tarfile
import tempfile
import unittest
import zipfile

from perfetto.bigtrace.server import TraceCatalog
from perfetto.bigtrace.server.archive import extract_archive, is_archive
from test.bigtrace_server_fixture import COLD_TRACE, HOT_TRACE, trace_path


def write_tar(path: str, mode: str, files: dict) -> None:
  with tarfile.open(path, mode) as tf:
    for name, data in files.items():
      info = tarfile.TarInfo(name)
      info.size = len(data)
      tf.addfile(info, io.BytesIO(data))


def write_zip(path: str, files: dict) -> None:
  with zipfile.ZipFile(path, "w") as zf:
    for name, data in files.items():
      zf.writestr(name, data)


class BigtraceServerArchiveTest(unittest.TestCase):

  def setUp(self):
    tmp = tempfile.TemporaryDirectory()
    self.addCleanup(tmp.cleanup)
    self.tmp_dir = tmp.name
    self.cache_dir = os.path.join(self.tmp_dir, "cache")

  def _path(self, name: str) -> str:
    return os.path.join(self.tmp_dir, name)

  def _extract(self, path: str) -> str:
    return extract_archive(path, cache_dir=self.cache_dir)

  def test_formats_are_detected_by_content(self):
    files = {"a/trace": b"data"}
    # Misleading names: the format comes from the bytes, not the extension.
    write_zip(self._path("zip.bin"), files)
    write_tar(self._path("tar.bin"), "w", files)
    write_tar(self._path("targz.zip"), "w:gz", files)
    write_tar(self._path("tarxz"), "w:xz", files)
    for name in ("zip.bin", "tar.bin", "targz.zip", "tarxz"):
      with self.subTest(name):
        self.assertTrue(is_archive(self._path(name)))
        out = self._extract(self._path(name))
        with open(os.path.join(out, "a", "trace"), "rb") as f:
          self.assertEqual(f.read(), b"data")

  def test_traces_and_directories_are_not_archives(self):
    self.assertFalse(is_archive(trace_path(COLD_TRACE)))
    self.assertFalse(is_archive(trace_path("android_anr.pftrace.gz")))
    self.assertFalse(is_archive(self.tmp_dir))
    self.assertFalse(is_archive(self._path("missing")))

  def test_entries_escaping_the_output_are_skipped(self):
    files = {"../evil": b"x", "/abs": b"x", "ok": b"ok"}
    write_zip(self._path("t.zip"), files)
    write_tar(self._path("t.tar"), "w", files)
    for name in ("t.zip", "t.tar"):
      with self.subTest(name):
        out = self._extract(self._path(name))
        self.assertEqual(os.listdir(out), ["ok"])
    self.assertFalse(os.path.exists(os.path.join(self.cache_dir, "evil")))

  def test_extraction_is_reused_until_the_archive_changes(self):
    path = self._path("t.tar.gz")
    write_tar(path, "w:gz", {"a": b"1"})
    first = self._extract(path)
    self.assertEqual(self._extract(path), first)

    write_tar(path, "w:gz", {"a": b"1", "b": b"22"})
    second = self._extract(path)
    self.assertNotEqual(second, first)
    self.assertEqual(sorted(os.listdir(second)), ["a", "b"])

  def test_catalog_serves_traces_from_extracted_archive(self):
    path = self._path("traces.tgz")
    with tarfile.open(path, "w:gz") as tf:
      tf.add(trace_path(COLD_TRACE), f"run1/{COLD_TRACE}")
      tf.add(trace_path(HOT_TRACE), f"run2/{HOT_TRACE}")
    catalog = TraceCatalog(trace_dir=self._extract(path), recursive=True)
    names = sorted(t.file_name for t in catalog.get_traces())
    self.assertEqual(names, [COLD_TRACE, HOT_TRACE])


if __name__ == "__main__":
  unittest.main()
