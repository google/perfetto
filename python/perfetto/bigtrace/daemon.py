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
"""The local Bigtrace server as an always-on systemd user service.

`install` writes a config file and a systemd user unit that runs `serve` with
it, and starts it. `start`, `stop`, `restart`, `status`, `logs` and
`uninstall` control it. `tunnel`, run on another machine (e.g. a laptop),
forwards the server's port over SSH, so a browser there reaches a server on a
remote machine (e.g. a cloud workstation) at the same loopback address.

Nothing here imports DuckDB: `tunnel` and `status` work without it.
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from perfetto.bigtrace import paths

SERVICE = "perfetto-bigtrace.service"
DEFAULT_PORT = 5001
DEFAULT_TRACES = os.path.expanduser("~/traces")
UI_ORIGIN = "https://ui.perfetto.dev"
# Exit status of `run` for a bad config: systemd does not restart on it.
EX_CONFIG = 78
# `status` exit codes, as systemd's (LSB): running, failed, not running.
STATUS_OK, STATUS_UNHEALTHY, STATUS_STOPPED, STATUS_NOT_INSTALLED = 0, 1, 3, 4
# `tunnel` gives up after this many ssh runs in a row that end within 30s
# (e.g. authentication failures), rather than retrying forever.
TUNNEL_MAX_QUICK_FAILURES = 5

COMMANDS = ("install", "uninstall", "start", "stop", "restart", "status",
            "logs", "run", "tunnel")


class Config:
  """Settings of the installed service, kept in paths.CONFIG_FILE."""

  FIELDS = {
      "traces": DEFAULT_TRACES,
      "recursive": True,
      "port": DEFAULT_PORT,
      "workers": None,
      "cors_origins": [],
      "shell_path": None,
      "duckdb_path": os.path.join(paths.DATA_DIR, "bigtrace.duckdb"),
      "origin": UI_ORIGIN,
  }

  def __init__(self, **values):
    unknown = set(values) - set(self.FIELDS)
    if unknown:
      raise ValueError(f"unknown config keys: {', '.join(sorted(unknown))}")
    for key, default in self.FIELDS.items():
      value = values.get(key, default)
      setattr(self, key, list(value) if isinstance(value, list) else value)

  @classmethod
  def load(cls, path=None):
    with open(path or paths.CONFIG_FILE, encoding="utf-8") as f:
      data = json.load(f)
    data.pop("version", None)
    return cls(**data)

  def save(self, path=None):
    path = path or paths.CONFIG_FILE
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(
        os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600),
        "w",
        encoding="utf-8") as f:
      json.dump({"version": 1, **self.to_dict()}, f, indent=2)
      f.write("\n")
    os.replace(tmp, path)

  def to_dict(self):
    return {key: getattr(self, key) for key in self.FIELDS}

  def url(self):
    return f"http://127.0.0.1:{self.port}"

  def serve_args(self):
    """The `serve` command line of this config."""
    args = [
        self.traces, "--port",
        str(self.port), "--duckdb-path", self.duckdb_path, "--origin",
        self.origin
    ]
    args += ["--recursive"] * bool(self.recursive)
    args += ["--workers", str(self.workers)] if self.workers else []
    args += ["--shell-path", self.shell_path] if self.shell_path else []
    for origin in self.cors_origins:
      args += ["--cors-origin", origin]
    return args


def normalize_origin(origin):
  """`scheme://host[:port]` of a website origin, or ValueError."""
  u = urllib.parse.urlsplit(origin.strip())
  if u.scheme not in ("http", "https") or not u.hostname or u.username or (
      u.path not in ("", "/")) or u.query or u.fragment:
    raise ValueError(f"not a website origin (https://host[:port]): {origin!r}")
  return f"{u.scheme}://{u.netloc.lower()}"


def sd_notify(state):
  """Tells systemd about the service's state, if it runs under systemd."""
  addr = os.environ.get("NOTIFY_SOCKET")
  if not addr:
    return
  if addr[0] == "@":  # Abstract namespace socket.
    addr = "\0" + addr[1:]
  try:
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
      s.connect(addr)
      s.sendall(state.encode())
  except OSError:
    pass  # Readiness is best effort: systemd times the start out otherwise.


def systemd_quote(arg):
  """Quotes one ExecStart= / Environment= word for a systemd unit file."""
  escaped = (
      arg.replace("\\", "\\\\").replace('"',
                                        '\\"').replace("%",
                                                       "%%").replace("$", "$$"))
  return f'"{escaped}"'


def unit_path():
  return os.path.join(paths.UNIT_DIR, SERVICE)


def unit_text(config_path, python=None):
  """The systemd user unit running `run --config config_path`."""
  python = python or sys.executable
  # The directory holding the `perfetto` package, for checkouts that are not
  # on the interpreter's default path.
  package_root = os.path.dirname(
      os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
  exec_start = " ".join(
      systemd_quote(a) for a in
      [python, "-m", "perfetto.bigtrace", "run", "--config", config_path])
  return f"""\
# Written by `bigtrace install`: rerun it to change the settings rather than
# editing this file.
[Unit]
Description=Perfetto local Bigtrace server
Documentation=https://perfetto.dev/docs/analysis/bigtrace-local
StartLimitIntervalSec=120
StartLimitBurst=5

[Service]
Type=notify
ExecStart={exec_start}
Environment={systemd_quote("PYTHONPATH=" + package_root)}
Environment=PYTHONUNBUFFERED=1
Restart=on-failure
RestartSec=5
RestartPreventExitStatus={EX_CONFIG}
# The first start builds the DuckDB extension and downloads
# trace_processor_shell; `install` does that ahead, so this is a backstop.
TimeoutStartSec=900
TimeoutStopSec=30

[Install]
WantedBy=default.target
"""


def systemctl(*args, check=True):
  """Runs `systemctl --user ARGS`; returns the completed process."""
  try:
    proc = subprocess.run(["systemctl", "--user", *args],
                          capture_output=True,
                          text=True)
  except FileNotFoundError:
    raise SystemExit("error: systemctl not found: the service needs systemd")
  if check and proc.returncode:
    detail = (proc.stderr or proc.stdout).strip()
    raise SystemExit(f"error: systemctl --user {' '.join(args)} failed: "
                     f"{detail}")
  return proc


def require_systemd():
  if not sys.platform.startswith("linux"):
    raise SystemExit(
        "error: the Bigtrace server runs on Linux. On this machine, use "
        "`bigtrace tunnel HOST` to reach the server of a Linux machine.")
  if not shutil.which("systemctl"):
    raise SystemExit("error: systemctl not found: the service needs systemd "
                     "(run `bigtrace serve TRACES` in the foreground instead)")
  proc = systemctl("show-environment", check=False)
  if proc.returncode:
    raise SystemExit(
        "error: no systemd user session (" +
        (proc.stderr.strip() or "systemctl --user failed") +
        "). Log in normally (not with `su`), or run `bigtrace serve TRACES` "
        "in the foreground instead.")


def service_state():
  """ActiveState, SubState and MainPID of the service."""
  out = systemctl(
      "show",
      SERVICE,
      "--property=ActiveState,SubState,MainPID,LoadState",
      check=False).stdout
  props = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
  return props


def lingering():
  """Whether systemd keeps the user's services running without a session."""
  try:
    out = subprocess.run(
        ["loginctl", "show-user",
         str(os.getuid()), "--property=Linger"],
        capture_output=True,
        text=True).stdout
  except FileNotFoundError:
    return None
  return {"Linger=yes": True, "Linger=no": False}.get(out.strip())


def probe(url, timeout=2.0):
  """The server's `GET /` info, or None if it does not answer as Bigtrace."""
  try:
    with urllib.request.urlopen(f"{url}/", timeout=timeout) as response:
      info = json.load(response)
  except (OSError, ValueError):
    return None
  return info if isinstance(
      info, dict) and info.get("service") == "bigtrace" else None


def port_free(port):
  with socket.socket() as s:
    try:
      s.bind(("127.0.0.1", port))
    except OSError:
      return False
  return True


def remote_hint(port):
  """How to reach this machine's server from the one the user SSHed from."""
  if not os.environ.get("SSH_CONNECTION"):
    return None
  return (f"ssh -N -L {port}:127.0.0.1:{port} {socket.getfqdn()}   "
          f"(or: bigtrace tunnel {socket.getfqdn()})")


# Commands ---------------------------------------------------------------------


def add_commands(cmds, fmt):
  """Adds the service commands to the CLI's subparsers."""
  install = cmds.add_parser(
      "install",
      help="install and start the server as a user service",
      description=INSTALL_HELP,
      formatter_class=fmt)
  install.add_argument(
      "--traces",
      help="directory of traces to serve; the UI can pick any directory "
      f"inside it (default: {DEFAULT_TRACES}, created if missing)")
  install.add_argument(
      "--port", type=int, help=f"port (default: {DEFAULT_PORT})")
  install.add_argument(
      "--cors-origin",
      action="append",
      default=[],
      help="another Bigtrace UI website allowed to use the server, e.g. an "
      "internal deployment; repeatable, added to those installed before")
  install.add_argument(
      "--reset-cors-origins",
      action="store_true",
      help="forget the --cors-origin websites installed before")
  install.add_argument(
      "--workers",
      type=int,
      help="traces loaded at a time (default: CPUs, at most one per 2 GiB)")
  install.add_argument(
      "--shell-path",
      help="trace_processor_shell (default: the one pinned by this package, "
      "downloaded once)")
  install.add_argument(
      "--duckdb-path",
      help="DuckDB file keeping result tables and history "
      f"(default: {Config.FIELDS['duckdb_path']})")
  install.add_argument(
      "--no-recursive",
      action="store_true",
      help="serve only the top-level traces of --traces")
  install.add_argument(
      "--no-start",
      action="store_true",
      help="install without starting the service")
  install.set_defaults(main=install_main)

  uninstall = cmds.add_parser(
      "uninstall",
      help="stop and remove the user service",
      formatter_class=fmt,
      description="Stops the service and removes its unit. Traces are never "
      "touched.")
  uninstall.add_argument(
      "--purge",
      action="store_true",
      help="also delete the settings and the DuckDB database (result tables "
      "and query history)")
  uninstall.set_defaults(main=uninstall_main)

  for verb in ("start", "stop", "restart"):
    cmd = cmds.add_parser(verb, help=f"{verb} the installed service")
    cmd.set_defaults(main=lambda args, verb=verb: control_main(verb))

  status = cmds.add_parser(
      "status",
      help="show whether the service runs and what it serves",
      description="Exit status: 0 running, 1 running but not answering, "
      "3 stopped, 4 not installed.")
  status.add_argument(
      "--json", action="store_true", help="print the status as JSON")
  status.set_defaults(main=status_main)

  logs = cmds.add_parser("logs", help="show the service's log")
  logs.add_argument(
      "-f", "--follow", action="store_true", help="keep printing new lines")
  logs.add_argument(
      "-n",
      "--lines",
      type=int,
      default=100,
      help="lines to show (default: %(default)s)")
  logs.set_defaults(main=logs_main)

  run = cmds.add_parser(
      "run",
      help="run the server in the foreground with the installed settings "
      "(what the service does)")
  run.add_argument(
      "--config", default=paths.CONFIG_FILE, help=argparse.SUPPRESS)
  run.set_defaults(main=run_main)

  tunnel = cmds.add_parser(
      "tunnel",
      help="reach the server of a remote machine over SSH",
      description=TUNNEL_HELP,
      formatter_class=fmt)
  tunnel.add_argument("host", help="SSH destination, e.g. user@host")
  tunnel.add_argument(
      "--port",
      type=int,
      default=DEFAULT_PORT,
      help="port of the remote server (default: %(default)s)")
  tunnel.add_argument(
      "--local-port", type=int, help="local port to forward (default: --port)")
  tunnel.add_argument(
      "--ssh-option",
      action="append",
      default=[],
      metavar="OPTION",
      help="extra `ssh -o` option; repeatable")
  tunnel.set_defaults(main=tunnel_main)


INSTALL_HELP = """\
Installs the Bigtrace server as a systemd user service that is always on:
started now, at login and, with `loginctl enable-linger`, at boot; restarted
if it fails. Rerun to change settings: options not given keep their value.

The server only listens on 127.0.0.1. Open a Bigtrace UI and select the
`Local server` (http://127.0.0.1:PORT/v1); from another machine, run
`bigtrace tunnel THIS_HOST` there first.
"""

TUNNEL_HELP = """\
Forwards 127.0.0.1:PORT here to the Bigtrace server on HOST over SSH, so a
browser on this machine (e.g. a laptop) uses a server on HOST (e.g. a cloud
workstation) as its `Local server`. Reconnects until stopped with Ctrl-C.
"""


def install_main(args):
  require_systemd()
  config_path = paths.CONFIG_FILE
  cfg = Config.load(config_path) if os.path.exists(config_path) else Config()
  if args.traces:
    cfg.traces = os.path.abspath(os.path.expanduser(args.traces))
  for key in ("port", "workers", "shell_path", "duckdb_path"):
    if getattr(args, key) is not None:
      setattr(cfg, key, getattr(args, key))
  if args.shell_path:
    cfg.shell_path = os.path.abspath(os.path.expanduser(args.shell_path))
  if args.duckdb_path:
    cfg.duckdb_path = os.path.abspath(os.path.expanduser(args.duckdb_path))
  if args.no_recursive:
    cfg.recursive = False
  try:
    origins = [] if args.reset_cors_origins else cfg.cors_origins
    for origin in args.cors_origin:
      origins.append(normalize_origin(origin))
  except ValueError as e:
    raise SystemExit(f"error: {e}")
  cfg.cors_origins = list(dict.fromkeys(origins))
  if not 0 < cfg.port < 65536:
    raise SystemExit(f"error: bad --port {cfg.port}")
  if cfg.shell_path and not os.access(cfg.shell_path, os.X_OK):
    raise SystemExit(f"error: --shell-path {cfg.shell_path} is not executable")
  if os.path.exists(cfg.traces) and not os.path.isdir(cfg.traces):
    raise SystemExit(f"error: --traces {cfg.traces} is not a directory")

  running = service_state().get("ActiveState") in ("active", "activating")
  if not running and not port_free(cfg.port):
    raise SystemExit(
        f"error: port {cfg.port} is in use (another `bigtrace serve`?); "
        "stop it or pass --port")

  prepare(cfg)
  os.makedirs(cfg.traces, exist_ok=True)
  os.makedirs(os.path.dirname(cfg.duckdb_path), exist_ok=True)
  cfg.save(config_path)
  os.makedirs(paths.UNIT_DIR, exist_ok=True)
  with open(unit_path(), "w", encoding="utf-8") as f:
    f.write(unit_text(config_path))
  systemctl("daemon-reload")
  systemctl("enable", SERVICE)
  print(f"Installed {SERVICE} (settings: {config_path})")
  if args.no_start:
    print("Start it with `bigtrace start`.")
    return 0
  print("Starting...", flush=True)
  return control_main("restart")


def prepare(cfg):
  """Does the slow first-start work now, where errors are visible."""
  print(
      "Preparing the server (the first time, this builds its DuckDB "
      "extension and downloads trace_processor_shell)...",
      flush=True)
  try:
    import duckdb  # pylint: disable=import-outside-toplevel
    from perfetto.bigtrace import server  # pylint: disable=import-outside-toplevel
  except ModuleNotFoundError as e:
    raise SystemExit(f"error: {e.name} is missing: pip install "
                     f"'perfetto[bigtrace]' (or pip install duckdb pyarrow)")
  server.load_extension(
      duckdb.connect(config={"allow_unsigned_extensions": "true"}))
  if not cfg.shell_path:
    from perfetto.trace_processor.platform import PlatformDelegate  # pylint: disable=import-outside-toplevel
    PlatformDelegate().get_shell_path(bin_path=None)


def require_installed():
  if not os.path.exists(unit_path()):
    raise SystemExit("error: the Bigtrace service is not installed: run "
                     "`bigtrace install`")


def control_main(verb):
  require_systemd()
  require_installed()
  # Type=notify: start and restart return once the server answers, or fail.
  proc = systemctl(verb, SERVICE, check=False)
  if proc.returncode:
    print(proc.stderr.strip(), file=sys.stderr)
    print("The service failed to start; see `bigtrace logs`.", file=sys.stderr)
    return 1
  if verb == "stop":
    print("Stopped.")
    return 0
  return status_main(argparse.Namespace(json=False))


def status_main(args):
  installed = os.path.exists(unit_path()) and os.path.exists(paths.CONFIG_FILE)
  cfg = Config.load() if installed else None
  state = service_state() if installed and shutil.which("systemctl") else {}
  active = state.get("ActiveState", "inactive")
  info = probe(cfg.url()) if cfg and active == "active" else None
  if not installed:
    code = STATUS_NOT_INSTALLED
  elif active != "active":
    code = STATUS_STOPPED
  else:
    code = STATUS_OK if info else STATUS_UNHEALTHY
  status = {
      "installed": installed,
      "state": active if installed else "not-installed",
      "pid": int(state.get("MainPID") or 0) or None,
      "healthy": info is not None,
      "url": cfg and cfg.url(),
      "endpoint": cfg and f"{cfg.url()}/v1",
      "traceCount": info and info.get("traceCount"),
      "traceDir": info.get("traceDir") if info else cfg and cfg.traces,
      "corsOrigins": cfg and cfg.cors_origins,
      "config": paths.CONFIG_FILE if installed else None,
      "lingering": lingering() if installed else None,
  }
  if args.json:
    print(json.dumps(status, indent=2))
    return code
  if not installed:
    print("The Bigtrace service is not installed: run `bigtrace install`.")
    return code
  label = {
      STATUS_OK: "running",
      STATUS_UNHEALTHY: f"{active}, not answering yet",
      STATUS_STOPPED: f"{active} (start it with `bigtrace start`)",
  }[code]
  print(f"Bigtrace server: {label}")
  print(f"  Endpoint: {status['endpoint']}")
  traces = status["traceDir"]
  if info:
    traces = f"{info.get('traceCount')} traces in {traces}"
  print(f"  Traces:   {traces}")
  print(f"  Settings: {paths.CONFIG_FILE}  (change: bigtrace install ...)")
  print("  Logs:     bigtrace logs")
  if code == STATUS_OK:
    pick = ("select `Local server`" if cfg.port == DEFAULT_PORT else
            f"add the server {status['endpoint']}")
    print(f"\nIn a Bigtrace UI, {pick}, or open "
          f"{cfg.origin}/bigtrace.html?server={status['endpoint']}")
    hint = remote_hint(cfg.port)
    if hint:
      print(f"From your local machine, forward the port first:\n  {hint}")
  if status["lingering"] is False:
    print("\nThe service stops when you log out. To keep it running (and "
          "start it at boot): loginctl enable-linger")
  if code == STATUS_UNHEALTHY:
    print("\nSee `bigtrace logs`.")
  return code


def logs_main(args):
  require_installed()
  cmd = [
      "journalctl", "--user", "--unit", SERVICE, "--output", "cat", "--lines",
      str(args.lines)
  ]
  if args.follow:
    cmd.append("--follow")
  else:
    cmd.append("--no-pager")
  try:
    os.execvp(cmd[0], cmd)
  except FileNotFoundError:
    raise SystemExit("error: journalctl not found")


def uninstall_main(args):
  require_systemd()
  cfg = Config.load() if os.path.exists(paths.CONFIG_FILE) else None
  removed = []
  if os.path.exists(unit_path()):
    systemctl("disable", "--now", SERVICE, check=False)
    os.remove(unit_path())
    systemctl("daemon-reload")
    systemctl("reset-failed", SERVICE, check=False)
    removed.append(unit_path())
  if args.purge:
    files = [paths.CONFIG_FILE]
    if cfg:
      files += [cfg.duckdb_path, f"{cfg.duckdb_path}.wal"]
    for path in files:
      if os.path.exists(path):
        os.remove(path)
        removed.append(path)
  if not removed:
    print("The Bigtrace service is not installed.")
    return 0
  print("Removed:\n  " + "\n  ".join(removed))
  if cfg:
    print(f"The traces in {cfg.traces} are untouched.")
  if not args.purge and cfg:
    print(f"Kept the settings ({paths.CONFIG_FILE}) and result tables "
          f"({cfg.duckdb_path}); --purge deletes them.")
  return 0


def run_main(args):
  """Runs `serve` with the installed config, in the foreground."""
  try:
    cfg = Config.load(args.config)
  except (OSError, ValueError, TypeError) as e:
    print(f"error: cannot read {args.config}: {e}", file=sys.stderr)
    return EX_CONFIG
  if not os.path.isdir(cfg.traces):
    if cfg.traces != DEFAULT_TRACES:
      print(
          f"error: {cfg.traces} does not exist: create it or rerun "
          "`bigtrace install --traces DIR`",
          file=sys.stderr)
      return EX_CONFIG
    os.makedirs(cfg.traces)
  from perfetto.bigtrace import cli  # pylint: disable=import-outside-toplevel
  return cli.main(["serve", *cfg.serve_args()])


def tunnel_main(args):
  if not shutil.which("ssh"):
    raise SystemExit("error: ssh not found")
  local = args.local_port or args.port
  url = f"http://127.0.0.1:{local}"
  if not port_free(local):
    info = probe(url)
    where = (f"a Bigtrace server ({info.get('traceDir')}) already answers"
             if info else "something else uses it")
    raise SystemExit(f"error: port {local} is busy: {where}; pass "
                     "--local-port")
  cmd = ["ssh", "-N"]
  for option in [
      "ExitOnForwardFailure=yes", "ServerAliveInterval=15",
      "ServerAliveCountMax=3", *args.ssh_option
  ]:
    cmd += ["-o", option]
  cmd += ["-L", f"127.0.0.1:{local}:127.0.0.1:{args.port}", args.host]
  print(
      f"Forwarding {url} to port {args.port} of {args.host}; Ctrl-C stops.",
      flush=True)
  backoff, quick_failures = 1, 0
  ssh = None
  # Stopped by Ctrl-C or SIGTERM: either way, ssh goes too.
  signal.signal(signal.SIGTERM, signal.default_int_handler)
  try:
    while True:
      started = time.monotonic()
      ssh = subprocess.Popen(cmd)
      report_tunnel(ssh, url, args.host)
      code = ssh.wait()
      if time.monotonic() - started > 30:
        backoff, quick_failures = 1, 0
      else:
        quick_failures += 1
      if quick_failures >= TUNNEL_MAX_QUICK_FAILURES:
        print(
            f"error: ssh keeps failing (status {code}); check that "
            f"`ssh {args.host}` works",
            file=sys.stderr)
        return 1
      print(
          f"ssh exited with status {code}; reconnecting in {backoff}s",
          file=sys.stderr)
      time.sleep(backoff)
      backoff = min(backoff * 2, 60)
  except KeyboardInterrupt:
    return 0
  finally:
    if ssh and ssh.poll() is None:
      ssh.terminate()
      try:
        ssh.wait(timeout=5)
      except subprocess.TimeoutExpired:
        ssh.kill()


def report_tunnel(ssh, url, host):
  """Says whether the server answers through the tunnel, once it is up."""
  port = int(url.rsplit(":", 1)[1])
  deadline = time.monotonic() + 60
  answered_at = None  # When ssh started listening.
  while ssh.poll() is None and time.monotonic() < deadline:
    if answered_at is None and not port_free(port):
      answered_at = time.monotonic()
    if answered_at is not None:
      info = probe(url)
      if info:
        print(
            f"Connected: {info.get('traceCount')} traces in "
            f"{info.get('traceDir')} on {host}. In a Bigtrace UI, select "
            f"`Local server` ({url}/v1).",
            flush=True)
        return
      if time.monotonic() - answered_at > 10:
        print(
            f"The tunnel is up but no Bigtrace server answers on {host}: run "
            "`bigtrace status` there.",
            file=sys.stderr)
        return
    time.sleep(0.5)
