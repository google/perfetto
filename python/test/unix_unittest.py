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

import os
import tempfile
import unittest
from unittest import mock

from perfetto.trace_processor.unix import unix_socket_path_for


class TestUnixSocketPathFor(unittest.TestCase):

  def test_sock_suffix_used_as_is(self):
    self.assertEqual(unix_socket_path_for('foo.sock'), 'foo.sock')

  def test_absolute_path_used_as_is(self):
    self.assertEqual(unix_socket_path_for('/tmp/x'), '/tmp/x')

  @mock.patch.dict(os.environ, {'XDG_RUNTIME_DIR': '/run/user/42'})
  def test_session_name_uses_xdg_runtime_dir(self):
    self.assertEqual(
        unix_socket_path_for('my-session'),
        '/run/user/42/perfetto/my-session.sock')

  @mock.patch.dict(os.environ, {'XDG_RUNTIME_DIR': ''})
  def test_session_name_falls_back_to_temp_dir(self):
    self.assertEqual(
        unix_socket_path_for('my-session'),
        os.path.join(tempfile.gettempdir(), 'perfetto', 'my-session.sock'))

  def test_session_name_max_length(self):
    self.assertIsNotNone(unix_socket_path_for('a' * 64))
    self.assertIsNone(unix_socket_path_for('a' * 65))

  def test_not_a_unix_socket(self):
    for remote in [
        'localhost:9001',
        'http://localhost:9001',
        '-starts-with-dash',
        'has space',
        '',
    ]:
      with self.subTest(remote=remote):
        self.assertIsNone(unix_socket_path_for(remote))


if __name__ == '__main__':
  unittest.main()
