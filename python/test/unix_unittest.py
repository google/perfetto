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
import socket
import tempfile
import threading
import unittest
from unittest import mock

from perfetto.common.exceptions import PerfettoException
from perfetto.trace_processor.api import TraceProcessor
from perfetto.trace_processor.platform import PlatformDelegate
from perfetto.trace_processor.protos import ProtoFactory
from perfetto.trace_processor.unix import TraceProcessorUnix
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


class FakeServer:
  """A Unix socket server that answers the first request with fixed bytes."""

  def __init__(self, path: str, reply: bytes, keep_open: bool = False):
    self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    self._sock.bind(path)
    self._sock.listen(1)
    self._reply = reply
    self._keep_open = keep_open
    self._done = threading.Event()
    self._thread = threading.Thread(target=self._serve)
    self._thread.start()

  def _serve(self):
    conn, _ = self._sock.accept()
    conn.recv(4096)
    conn.sendall(self._reply)
    if self._keep_open:
      self._done.wait(10)
    conn.close()

  def stop(self):
    self._done.set()
    self._thread.join()
    self._sock.close()


@unittest.skipUnless(hasattr(socket, 'AF_UNIX'), 'needs AF_UNIX')
class TestRpcErrors(unittest.TestCase):

  def setUp(self):
    self.protos = ProtoFactory(PlatformDelegate())
    tmp_dir = tempfile.TemporaryDirectory()
    self.addCleanup(tmp_dir.cleanup)
    self.path = os.path.join(tmp_dir.name, 'fake.sock')

  def _query_with_reply(self, reply: bytes, keep_open: bool = False):
    server = FakeServer(self.path, reply, keep_open)
    self.addCleanup(server.stop)
    client = TraceProcessorUnix(self.path, protos=self.protos)
    self.addCleanup(client.close)
    client.execute_query('select 1')

  def _framed(self, **fields) -> bytes:
    stream = self.protos.TraceProcessorRpcStream()
    msg = stream.msg.add()
    for name, value in fields.items():
      setattr(msg, name, value)
    return stream.SerializeToString()

  def test_fatal_error(self):
    with self.assertRaisesRegex(PerfettoException, 'RPC framing error'):
      self._query_with_reply(self._framed(fatal_error='RPC framing error'))

  def test_invalid_request(self):
    # The server stays connected after this reply, so the client must not
    # wait for more messages.
    reply = self._framed(
        invalid_request=self.protos.TraceProcessorRpc.TPM_QUERY_STREAMING)
    with self.assertRaisesRegex(PerfettoException, 'does not support'):
      self._query_with_reply(reply, keep_open=True)

  def test_truncated_reply(self):
    # A header promising a 133-byte message, then only 50 bytes.
    reply = bytes([0x0A, 0x85, 0x01]) + b'x' * 50
    with self.assertRaisesRegex(ConnectionError, 'closed the connection'):
      self._query_with_reply(reply)


@unittest.skipUnless(hasattr(socket, 'AF_UNIX'), 'needs AF_UNIX')
class TestAddrIsHttpOnly(unittest.TestCase):

  def setUp(self):
    tmp_dir = tempfile.TemporaryDirectory()
    self.addCleanup(tmp_dir.cleanup)
    env_patch = mock.patch.dict(os.environ, {'XDG_RUNTIME_DIR': tmp_dir.name})
    env_patch.start()
    self.addCleanup(env_patch.stop)
    # A listening socket where a session called 'my-session' would be.
    path = unix_socket_path_for('my-session')
    os.makedirs(os.path.dirname(path))
    self.session = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    self.addCleanup(self.session.close)
    self.session.bind(path)
    self.session.listen(1)
    self.session.settimeout(0.5)

  def _session_got_connection(self) -> bool:
    try:
      conn, _ = self.session.accept()
    except socket.timeout:
      return False
    conn.close()
    return True

  def test_remote_uses_session(self):
    TraceProcessor(remote='my-session').close()
    self.assertTrue(self._session_got_connection())

  def test_addr_ignores_session(self):
    TraceProcessor(addr='my-session').close()
    self.assertFalse(self._session_got_connection())


if __name__ == '__main__':
  unittest.main()
