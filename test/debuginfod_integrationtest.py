#!/usr/bin/env python3
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
"""Local HTTP integration tests. Run with --shell PATH_TO_TRACE_PROCESSOR_SHELL.

Requires curl, llvm-symbolizer, and the downloaded test_symbolizer_binary
fixture. No public server is contacted.
"""

import argparse
import http.server
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD_ID = 'f7558cfad3e9e2ff6cafcb0fd8442a322210ba6e'
SHELL = None


class DebuginfodTest(unittest.TestCase):

  def setUp(self):
    self.temp = tempfile.TemporaryDirectory()
    self.addCleanup(self.temp.cleanup)
    self.root = Path(self.temp.name)
    self.cache = self.root / 'cache'
    self.requests = []
    binary = (ROOT / 'test/data/test_symbolizer_binary').read_bytes()
    requests = self.requests

    class Handler(http.server.BaseHTTPRequestHandler):

      def log_message(self, *_):
        pass

      def do_GET(self):
        requests.append(self.path)
        if self.path.startswith('/missing/'):
          self.send_error(404)
          return
        if self.path.startswith('/slow/'):
          time.sleep(2)
        if self.path.startswith('/redirect/'):
          self.send_response(302)
          self.send_header('Location', self.path.replace('/redirect/', '/ok/'))
          self.end_headers()
          return
        data = b'not an ELF file' if self.path.startswith('/bad/') else binary
        if self.path.startswith('/wrong/'):
          data = binary.replace(bytes.fromhex(BUILD_ID), b'\x00' * 20)
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try:
          self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
          pass

    self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    self.addCleanup(self.server.server_close)
    self.addCleanup(self.server.shutdown)
    thread = threading.Thread(target=self.server.serve_forever, daemon=True)
    thread.start()
    self.url = 'http://127.0.0.1:%d' % self.server.server_port
    self.env = dict(os.environ)
    for name in ('DEBUGINFOD_URLS', 'DEBUGINFOD_CACHE_PATH',
                 'LLVM_SYMBOLIZER_OPTS', 'PERFETTO_BINARY_PATH',
                 'BREAKPAD_SYMBOL_DIR'):
      self.env.pop(name, None)
    self.env['NO_PROXY'] = '127.0.0.1'
    self.env['no_proxy'] = '127.0.0.1'
    build_id = ''.join('\\%03o' % byte for byte in bytes.fromhex(BUILD_ID))
    source = self.root / 'trace.textproto'
    source.write_text('''packet { clock_snapshot {
      clocks { clock_id: 3 timestamp: 0 }
      clocks { clock_id: 5 timestamp: 0 }
      clocks { clock_id: 6 timestamp: 0 }
    } }
    packet {
      trusted_packet_sequence_id: 1
      incremental_state_cleared: true
      thread_descriptor { pid: 1 tid: 1 }
      interned_data {
        build_ids { iid: 1 str: "%s" }
        mapping_paths { iid: 1 str: "remote-only" }
        mappings { iid: 1 build_id: 1 path_string_ids: 1
                   start: 1048576 end: 1114112 load_bias: 0 exact_offset: 4096 }
        frames { iid: 1 mapping_id: 1 rel_pc: 4400 }
        callstacks { iid: 1 frame_ids: 1 }
      }
    }
    packet {
      trusted_packet_sequence_id: 1
      streaming_profile_packet { callstack_iid: 1 timestamp_delta_us: 1 }
    }''' % build_id)
    self.trace = self.root / 'trace.pftrace'
    self.run_shell('util', 'text_to_binary', str(source), str(self.trace))

  def run_shell(self, *args, success=True):
    result = subprocess.run([SHELL, *args],
                            env=self.env,
                            capture_output=True,
                            timeout=15)
    if success:
      self.assertEqual(result.returncode, 0, result.stderr.decode())
    else:
      self.assertNotEqual(result.returncode, 0)
    return result

  def bundle(self, *args):
    return self.run_shell('bundle', '--no-auto-symbol-paths',
                          '--no-auto-proguard-maps', '--debuginfod-cache-path',
                          str(self.cache), *args, str(self.trace),
                          str(self.root / 'bundle.tar'))

  def assert_symbolized(self):
    result = self.run_shell('query', '-q', str(self.root / 'bundle.tar'),
                            'SELECT name FROM stack_profile_symbol')
    self.assertIn(b'TestFunctionToSymbolize', result.stdout)

  def test_remote_only_and_cache_reuse(self):
    result = self.bundle('--debuginfod', '--debuginfod-urls', self.url + '/ok')
    self.assertIn(b'1 downloaded', result.stderr)
    self.assert_symbolized()
    self.assertEqual(self.requests, ['/ok/buildid/' + BUILD_ID + '/debuginfo'])
    self.requests.clear()
    result = self.bundle('--debuginfod', '--debuginfod-urls', self.url + '/ok')
    self.assertIn(b'1 cache hits', result.stderr)
    self.assertEqual(self.requests, [])

  def test_disabled_warns_even_when_quiet(self):
    self.env['DEBUGINFOD_URLS'] = self.url + '/ok'
    result = self.bundle('-q')
    self.assertIn(b'ignored', result.stderr)
    self.assertIn(b'--debuginfod', result.stderr)
    self.assertEqual(self.requests, [])
    self.assertFalse(self.cache.exists())

  def test_cli_overrides_environment_and_llvm_options(self):
    self.env['DEBUGINFOD_URLS'] = self.url + '/missing'
    self.env['DEBUGINFOD_CACHE_PATH'] = str(self.root / 'wrong-cache')
    self.env[
        'LLVM_SYMBOLIZER_OPTS'] = '--output-style=GNU --debuginfod --invalid-option'
    result = self.bundle('--debuginfod', '--debuginfod-urls',
                         self.url + '/redirect')
    self.assertIn(b'LLVM_SYMBOLIZER_OPTS is ignored', result.stderr)
    self.assert_symbolized()
    self.assertFalse((self.root / 'wrong-cache').exists())
    self.assertFalse(any('/missing/' in path for path in self.requests))

  def test_invalid_response_falls_back(self):
    result = self.bundle('--debuginfod', '--debuginfod-urls',
                         self.url + '/bad\t' + self.url + '/ok', '--verbose')
    self.assertIn(b'invalid debug file', result.stderr)
    self.assert_symbolized()
    self.assertEqual(len(self.requests), 2)
    self.assertEqual(list(self.cache.rglob('*.tmp.*')), [])

  def test_wrong_build_id_falls_back(self):
    self.bundle('--debuginfod', '--debuginfod-urls',
                self.url + '/wrong ' + self.url + '/ok')
    self.assert_symbolized()
    self.assertEqual(len(self.requests), 2)

  def test_invalid_cache_is_replaced(self):
    entry = self.cache / BUILD_ID / 'debuginfo'
    entry.parent.mkdir(parents=True)
    entry.write_bytes(b'incomplete')
    self.bundle('--debuginfod', '--debuginfod-urls', self.url + '/ok')
    self.assert_symbolized()
    self.assertEqual(len(self.requests), 1)
    self.assertEqual(entry.read_bytes(),
                     (ROOT / 'test/data/test_symbolizer_binary').read_bytes())

  def test_curl_config_cannot_add_requests(self):
    (self.root / '.curlrc').write_text('url = "' + self.url + '/missing"')
    self.env['CURL_HOME'] = str(self.root)
    self.bundle('--debuginfod', '--debuginfod-urls', self.url + '/ok')
    self.assert_symbolized()
    self.assertEqual(self.requests, ['/ok/buildid/' + BUILD_ID + '/debuginfo'])

  def test_failed_download_does_not_publish_cache_entry(self):
    result = self.bundle('--debuginfod', '--debuginfod-urls', self.url + '/bad')
    self.assertIn(b'1 unavailable', result.stderr)
    self.assertFalse((self.cache / BUILD_ID / 'debuginfo').exists())
    self.assertEqual(list(self.cache.rglob('*.tmp.*')), [])

  def test_local_result_prevents_download(self):
    local = self.root / 'local'
    local.mkdir()
    shutil.copy(ROOT / 'test/data/test_symbolizer_binary', local / 'binary')
    self.bundle('--debuginfod', '--debuginfod-urls', self.url + '/ok',
                '--symbol-paths', str(local))
    self.assert_symbolized()
    self.assertEqual(self.requests, [])

  def test_stall_timeout_falls_back(self):
    result = self.bundle('--debuginfod', '--debuginfod-stall-timeout', '1',
                         '--debuginfod-urls',
                         self.url + '/slow ' + self.url + '/ok', '--verbose')
    self.assertIn(b'curl exit 28', result.stderr)
    self.assert_symbolized()

  @unittest.skipUnless(hasattr(os, 'openpty'), 'requires a pseudo-terminal')
  def test_progress_controls(self):
    self.env['TERM'] = 'xterm'
    for flags in ([], ['--no-progress'], ['-q']):
      shutil.rmtree(self.cache, ignore_errors=True)
      master, slave = os.openpty()
      try:
        result = subprocess.run([
            SHELL, 'bundle', '--no-auto-symbol-paths',
            '--no-auto-proguard-maps', '--debuginfod', '--debuginfod-urls',
            self.url + '/ok', '--debuginfod-cache-path',
            str(self.cache), *flags,
            str(self.trace),
            str(self.root / 'bundle.tar')
        ],
                                env=self.env,
                                stdout=subprocess.PIPE,
                                stderr=slave,
                                timeout=15)
        os.close(slave)
        slave = None
        output = b''
        while True:
          try:
            data = os.read(master, 4096)
          except OSError:
            break
          if not data:
            break
          output += data
        self.assertEqual(result.returncode, 0, output.decode())
        self.assertEqual(b'Debuginfod: fetching build ID' in output, not flags)
        if flags == ['-q']:
          self.assertEqual(output, b'')
      finally:
        os.close(master)
        if slave is not None:
          os.close(slave)

  def test_symbolize_utility_and_traceconv(self):
    flags = [
        '--debuginfod', '--debuginfod-urls', self.url + '/ok',
        '--debuginfod-cache-path',
        str(self.cache), '--quiet'
    ]
    result = self.run_shell('util', 'symbolize', *flags, str(self.trace))
    self.assertIn(b'TestFunctionToSymbolize', result.stdout)
    self.assertEqual(result.stderr, b'')
    result = subprocess.run([
        str(Path(SHELL).with_name('traceconv')), 'symbolize', *flags,
        str(self.trace)
    ],
                            env=self.env,
                            capture_output=True,
                            timeout=15)
    self.assertEqual(result.returncode, 0, result.stderr.decode())
    self.assertIn(b'TestFunctionToSymbolize', result.stdout)
    self.assertEqual(result.stderr, b'')

  def test_query_and_classic_interface_without_local_binaries(self):
    for args in [('query', '-q', str(self.trace),
                  'SELECT name FROM stack_profile_symbol'),
                 ('--quiet', '-Q', 'SELECT name FROM stack_profile_symbol',
                  str(self.trace))]:
      result = self.run_shell(*args, '--debuginfod', '--debuginfod-urls',
                              self.url + '/ok', '--debuginfod-cache-path',
                              str(self.cache))
      self.assertIn(b'TestFunctionToSymbolize', result.stdout)
      self.assertEqual(result.stderr, b'', result.stderr.decode())


if __name__ == '__main__':
  parser = argparse.ArgumentParser()
  parser.add_argument('--shell', required=True)
  args, remaining = parser.parse_known_args()
  SHELL = str(Path(args.shell).resolve())
  unittest.main(argv=[__file__, *remaining])
