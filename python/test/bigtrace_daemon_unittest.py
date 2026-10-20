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
"""Tests of the local Bigtrace server's service commands (daemon.py).

systemd is faked; tools/install-bigtrace and a real systemd user session are
exercised by the steps in docs/analysis/bigtrace-local.md. These tests need
neither DuckDB nor systemd.
"""

import argparse
import contextlib
import http.server
import io
import json
import os
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import unittest.mock

from perfetto.bigtrace import daemon
from perfetto.bigtrace import paths


def install_args(**overrides):
  values = dict(
      traces=None,
      port=None,
      cors_origin=[],
      reset_cors_origins=False,
      workers=None,
      shell_path=None,
      duckdb_path=None,
      no_recursive=False,
      no_start=True)
  values.update(overrides)
  return argparse.Namespace(**values)


class FakeBigtrace(http.server.BaseHTTPRequestHandler):
  """Answers `GET /` like the Bigtrace server does."""

  def do_GET(self):  # pylint: disable=invalid-name
    body = json.dumps({
        "service": "bigtrace",
        "traceCount": 3,
        "traceDir": "/traces"
    }).encode()
    self.send_response(200)
    self.send_header("Content-Type", "application/json")
    self.send_header("Content-Length", str(len(body)))
    self.end_headers()
    self.wfile.write(body)

  def log_message(self, *args):
    pass


@contextlib.contextmanager
def fake_server():
  httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeBigtrace)
  thread = threading.Thread(target=httpd.serve_forever, daemon=True)
  thread.start()
  try:
    yield httpd.server_port
  finally:
    httpd.shutdown()
    httpd.server_close()


# A stand-in for `ssh -N ... -L 127.0.0.1:LOCAL:127.0.0.1:REMOTE HOST` that
# forwards LOCAL to REMOTE on this machine.
FAKE_SSH = r"""#!{python}
import socket, sys, threading
spec = sys.argv[sys.argv.index("-L") + 1].split(":")
local, remote = int(spec[1]), int(spec[3])
srv = socket.create_server(("127.0.0.1", local))
def pipe(a, b):
  try:
    while (data := a.recv(65536)):
      b.sendall(data)
  except OSError:
    pass
  finally:
    b.close()
while True:
  conn, _ = srv.accept()
  up = socket.create_connection(("127.0.0.1", remote))
  threading.Thread(target=pipe, args=(conn, up), daemon=True).start()
  threading.Thread(target=pipe, args=(up, conn), daemon=True).start()
"""


def free_port():
  with socket.socket() as s:
    s.bind(("127.0.0.1", 0))
    return s.getsockname()[1]


class DaemonTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.mkdtemp()
    self.addCleanup(subprocess.run, ["rm", "-rf", self.tmp])
    for name, value in (("CONFIG_FILE", f"{self.tmp}/config/bigtrace.json"),
                        ("UNIT_DIR", f"{self.tmp}/units"),
                        ("DATA_DIR", f"{self.tmp}/data")):
      patcher = unittest.mock.patch.object(paths, name, value)
      patcher.start()
      self.addCleanup(patcher.stop)
    self.calls = []
    self.state = {"ActiveState": "inactive", "SubState": "dead", "MainPID": "0"}
    for name, fake in (
        ("systemctl", self.fake_systemctl),
        ("require_systemd", lambda: None),
        ("prepare", lambda cfg: None),
        ("lingering", lambda: True),
    ):
      patcher = unittest.mock.patch.object(daemon, name, fake)
      patcher.start()
      self.addCleanup(patcher.stop)
    self.traces = os.path.join(self.tmp, "traces")
    # The default port may be taken by a real server on this machine.
    patcher = unittest.mock.patch.dict(daemon.Config.FIELDS,
                                       {"port": free_port()})
    patcher.start()
    self.addCleanup(patcher.stop)

  def fake_systemctl(self, *args, check=True):
    del check
    self.calls.append(args)
    if args[0] == "show":
      out = "".join(f"{k}={v}\n" for k, v in self.state.items())
      return subprocess.CompletedProcess(args, 0, out, "")
    return subprocess.CompletedProcess(args, 0, "", "")

  def run_quietly(self, fn, *args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
      code = fn(*args)
    return code, out.getvalue()

  # Config ---------------------------------------------------------------------

  def test_config_round_trips_and_builds_the_serve_command(self):
    cfg = daemon.Config(
        traces="/t",
        port=6000,
        workers=4,
        cors_origins=["https://a.example"],
        shell_path="/bin/tp",
        recursive=False)
    path = os.path.join(self.tmp, "c.json")
    cfg.save(path)
    self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
    self.assertEqual(daemon.Config.load(path).to_dict(), cfg.to_dict())
    self.assertEqual(cfg.serve_args(), [
        "/t", "--port", "6000", "--duckdb-path", cfg.duckdb_path, "--origin",
        "https://ui.perfetto.dev", "--workers", "4", "--shell-path", "/bin/tp",
        "--cors-origin", "https://a.example"
    ])
    self.assertIn("--recursive", daemon.Config(traces="/t").serve_args())

  def test_config_rejects_unknown_keys(self):
    with self.assertRaisesRegex(ValueError, "unknown config keys: bogus"):
      daemon.Config(bogus=1)

  def test_normalize_origin(self):
    self.assertEqual(
        daemon.normalize_origin("https://Brush-UI.example.com/"),
        "https://brush-ui.example.com")
    self.assertEqual(
        daemon.normalize_origin("http://localhost:10000"),
        "http://localhost:10000")
    for bad in ("brush-ui.example.com", "https://a.example/path",
                "https://a.example/?q", "ftp://a.example",
                "https://user@a.example", "javascript:alert(1)"):
      with self.assertRaises(ValueError, msg=bad):
        daemon.normalize_origin(bad)

  # Unit file ------------------------------------------------------------------

  def test_unit_quotes_paths_for_systemd(self):
    unit = daemon.unit_text('/a b/100%/$HOME/"q".json', python="/py thon")
    exec_start = next(
        l for l in unit.splitlines() if l.startswith("ExecStart="))
    self.assertEqual(
        exec_start, 'ExecStart="/py thon" "-m" "perfetto.bigtrace" "run" '
        r'"--config" "/a b/100%%/$$HOME/\"q\".json"')
    self.assertIn("Type=notify", unit)
    self.assertIn(f"RestartPreventExitStatus={daemon.EX_CONFIG}", unit)
    self.assertIn("WantedBy=default.target", unit)
    # The checkout's python/ directory, where `perfetto` is.
    root = os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(daemon.__file__))))
    self.assertIn(f'Environment="PYTHONPATH={root}"', unit)

  def test_sd_notify(self):
    sock_path = os.path.join(self.tmp, "notify")
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
      sock.bind(sock_path)
      with unittest.mock.patch.dict(os.environ, {"NOTIFY_SOCKET": sock_path}):
        daemon.sd_notify("READY=1")
      self.assertEqual(sock.recv(100), b"READY=1")
    with unittest.mock.patch.dict(os.environ, {}, clear=True):
      daemon.sd_notify("READY=1")  # Not under systemd: a no-op.

  # install / uninstall --------------------------------------------------------

  def test_install_writes_config_and_unit_and_enables_it(self):
    code, out = self.run_quietly(
        daemon.install_main,
        install_args(
            traces=self.traces,
            port=6001,
            cors_origin=["https://brush-ui.example.com/"]))
    self.assertEqual(code, 0, out)
    self.assertTrue(os.path.isdir(self.traces))
    cfg = daemon.Config.load()
    self.assertEqual(cfg.traces, self.traces)
    self.assertEqual(cfg.port, 6001)
    self.assertEqual(cfg.cors_origins, ["https://brush-ui.example.com"])
    with open(daemon.unit_path(), encoding="utf-8") as f:
      self.assertIn(paths.CONFIG_FILE, f.read())
    self.assertEqual(self.calls[-2:], [("daemon-reload",),
                                       ("enable", daemon.SERVICE)])

  def test_reinstall_keeps_settings_not_given(self):
    self.run_quietly(
        daemon.install_main,
        install_args(
            traces=self.traces, port=6002, cors_origin=["https://a.example"]))
    self.run_quietly(daemon.install_main,
                     install_args(cors_origin=["https://b.example"]))
    cfg = daemon.Config.load()
    self.assertEqual((cfg.traces, cfg.port), (self.traces, 6002))
    self.assertEqual(cfg.cors_origins,
                     ["https://a.example", "https://b.example"])
    self.run_quietly(
        daemon.install_main,
        install_args(
            reset_cors_origins=True, cors_origin=["https://c.example"]))
    self.assertEqual(daemon.Config.load().cors_origins, ["https://c.example"])

  def test_install_starts_the_service(self):
    with unittest.mock.patch.object(daemon, "probe", return_value=None):
      self.run_quietly(daemon.install_main,
                       install_args(traces=self.traces, no_start=False))
    self.assertIn(("restart", daemon.SERVICE), self.calls)

  def test_install_rejects_bad_settings(self):
    for args, message in (
        (install_args(cors_origin=["nope"]), "not a website origin"),
        (install_args(port=70000), "bad --port"),
        (install_args(shell_path="/does/not/exist"), "not executable"),
    ):
      with self.assertRaisesRegex(SystemExit, message):
        self.run_quietly(daemon.install_main, args)
    self.assertFalse(os.path.exists(paths.CONFIG_FILE))

  def test_install_refuses_a_busy_port(self):
    with socket.socket() as s:
      s.bind(("127.0.0.1", 0))
      s.listen()
      port = s.getsockname()[1]
      with self.assertRaisesRegex(SystemExit, f"port {port} is in use"):
        self.run_quietly(daemon.install_main,
                         install_args(traces=self.traces, port=port))

  def test_uninstall_keeps_data_unless_purged(self):
    self.run_quietly(daemon.install_main, install_args(traces=self.traces))
    db = daemon.Config.load().duckdb_path
    os.makedirs(os.path.dirname(db), exist_ok=True)
    open(db, "w").close()
    self.run_quietly(daemon.uninstall_main, argparse.Namespace(purge=False))
    self.assertFalse(os.path.exists(daemon.unit_path()))
    self.assertTrue(os.path.exists(db))
    self.assertIn(("disable", "--now", daemon.SERVICE), self.calls)
    self.run_quietly(daemon.uninstall_main, argparse.Namespace(purge=True))
    self.assertFalse(os.path.exists(db))
    self.assertFalse(os.path.exists(paths.CONFIG_FILE))
    self.assertTrue(os.path.isdir(self.traces))  # Traces are never touched.

  # status ---------------------------------------------------------------------

  def status(self):
    code, out = self.run_quietly(daemon.status_main,
                                 argparse.Namespace(json=True))
    return code, json.loads(out)

  def test_status(self):
    self.assertEqual(self.status()[0], daemon.STATUS_NOT_INSTALLED)
    with fake_server() as port:
      # Installing over the running service: its port is in use, by it.
      self.state.update(ActiveState="active")
      self.run_quietly(daemon.install_main,
                       install_args(traces=self.traces, port=port))
      self.state.update(ActiveState="inactive")
      self.assertEqual(self.status()[0], daemon.STATUS_STOPPED)
      self.state.update(ActiveState="active", MainPID="42")
      code, status = self.status()
      self.assertEqual(code, daemon.STATUS_OK)
      self.assertEqual(status["pid"], 42)
      self.assertEqual(status["traceCount"], 3)
      self.assertEqual(status["endpoint"], f"http://127.0.0.1:{port}/v1")
    # Running, but nothing answers any more.
    self.assertEqual(self.status()[0], daemon.STATUS_UNHEALTHY)

  # run ------------------------------------------------------------------------

  def test_run_serves_the_installed_config(self):
    self.run_quietly(daemon.install_main,
                     install_args(traces=self.traces, port=6003))
    with unittest.mock.patch(
        "perfetto.bigtrace.cli.main", return_value=0) as cli_main:
      self.assertEqual(
          daemon.run_main(argparse.Namespace(config=paths.CONFIG_FILE)), 0)
    cli_main.assert_called_once_with(
        ["serve", *daemon.Config.load().serve_args()])

  def test_run_fails_permanently_on_a_bad_config(self):
    missing = argparse.Namespace(config=os.path.join(self.tmp, "none.json"))
    code, _ = self.run_quietly(daemon.run_main, missing)
    self.assertEqual(code, daemon.EX_CONFIG)
    self.run_quietly(daemon.install_main, install_args(traces=self.traces))
    os.rmdir(self.traces)
    code, out = self.run_quietly(daemon.run_main,
                                 argparse.Namespace(config=paths.CONFIG_FILE))
    self.assertEqual(code, daemon.EX_CONFIG)
    self.assertIn("does not exist", out)

  # tunnel ---------------------------------------------------------------------

  def fake_ssh(self, script):
    bin_dir = os.path.join(self.tmp, "bin")
    os.makedirs(bin_dir, exist_ok=True)
    path = os.path.join(bin_dir, "ssh")
    with open(path, "w", encoding="utf-8") as f:
      f.write(script.replace("{python}", sys.executable))
    os.chmod(path, 0o755)
    return unittest.mock.patch.dict(
        os.environ, {"PATH": bin_dir + os.pathsep + os.environ["PATH"]})

  def test_tunnel_forwards_the_remote_server(self):
    local = free_port()
    with fake_server() as remote, self.fake_ssh(FAKE_SSH):
      tool = os.path.join(
          os.path.dirname(__file__), "..", "..", "tools", "bigtrace_server.py")
      proc = subprocess.Popen([
          sys.executable, tool, "tunnel", "host.example", "--port",
          str(remote), "--local-port",
          str(local)
      ],
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT,
                              text=True)
      try:
        lines = [proc.stdout.readline() for _ in range(2)]
      finally:
        proc.terminate()
        proc.wait()
        proc.stdout.close()
    self.assertIn(f"Forwarding http://127.0.0.1:{local}", lines[0])
    self.assertIn("Connected: 3 traces in /traces on host.example", lines[1])
    # Stopping the tunnel stops ssh, which frees the port.
    deadline = time.monotonic() + 10
    while not daemon.port_free(local) and time.monotonic() < deadline:
      time.sleep(0.1)
    self.assertTrue(daemon.port_free(local), "ssh outlived the tunnel")

  def test_tunnel_gives_up_when_ssh_keeps_failing(self):
    args = argparse.Namespace(
        host="host.example", port=free_port(), local_port=None, ssh_option=[])
    with self.fake_ssh("#!/bin/sh\nexit 255\n"), unittest.mock.patch.object(
        daemon.time, "sleep"):
      code, out = self.run_quietly(daemon.tunnel_main, args)
    self.assertEqual(code, 1)
    self.assertIn("ssh keeps failing (status 255)", out)

  def test_tunnel_refuses_a_busy_local_port(self):
    with fake_server() as port:
      args = argparse.Namespace(
          host="h", port=port, local_port=None, ssh_option=[])
      with self.fake_ssh("#!/bin/sh\nexit 0\n"), self.assertRaisesRegex(
          SystemExit, "a Bigtrace server .* already answers"):
        daemon.tunnel_main(args)


if __name__ == "__main__":
  unittest.main()
