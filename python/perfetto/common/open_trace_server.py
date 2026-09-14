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
"""Serves a trace file to the Perfetto UI, shared by //tools scripts."""

import http.server
import json
import os
import socket
import socketserver
import urllib.parse
import webbrowser

from perfetto.common.repo_utils import *

# We reuse the HTTP+RPC port because it's the only one allowed by the CSP.
PORT = 9001

BOOTSTRAP_PATH = 'python/perfetto/common/open_trace.html'


class HttpHandler(http.server.SimpleHTTPRequestHandler):

  def end_headers(self):
    self.send_header('Access-Control-Allow-Origin', self.server.allow_origin)
    self.send_header('Cache-Control', 'no-cache')
    super().end_headers()

  def do_GET(self):
    if self.path == '/' and self.server.bootstrap_html is not None:
      body = self.server.bootstrap_html.encode('utf-8')
      self.send_response(200)
      self.send_header('Content-Type', 'text/html; charset=utf-8')
      self.send_header('Content-Length', str(len(body)))
      self.end_headers()
      self.wfile.write(body)
      return

    if self.path != '/' + self.server.expected_fname:
      self.send_error(404, 'File not found')
      return

    # Mark the request as completed only after serving, so that the loop in
    # open_trace_in_browser() can serve the bootstrap page beforehand.
    super().do_GET()
    self.server.fname_get_completed = True

  def do_POST(self):
    self.send_error(404, 'File not found')


def _strip_url_path(url):
  """In case the user's `--origin` has a path component, strips it out."""
  parsed = urllib.parse.urlparse(url)
  return urllib.parse.urlunparse(
      parsed._replace(path='', query='', fragment='', params=''))


def _load_bootstrap_html(ui_url, fname):
  """Returns the bootstrap page HTML, or None if its source is unavailable.

  tools/record_android_trace is a standalone script that people copy outside of
  the repo. When that happens the .html file is not there and we just fall back
  to the regular flow.
  """
  try:
    with open(repo_dir(BOOTSTRAP_PATH), encoding='utf-8') as f:
      html = f.read()
  except OSError:
    return None
  html = html.replace('{{UI_URL}}', json.dumps(ui_url))
  return html.replace('{{FNAME}}', json.dumps(fname))


def open_trace_in_browser(path,
                          open_browser,
                          origin,
                          referrer,
                          url_params=None,
                          startup_commands=None,
                          remote_browser=False):
  path = os.path.abspath(path)
  fname = os.path.basename(path)

  params = [f'referrer={referrer}']
  params += list(url_params) if url_params else []
  if startup_commands:
    try:
      json.loads(startup_commands)  # Validate JSON format.
      params.append(f'startupCommands={urllib.parse.quote(startup_commands)}')
    except (json.JSONDecodeError, TypeError) as e:
      print(f'Warning: Invalid startup commands JSON, ignoring. Error: {e}')

  # A browser on another machine cannot reach 127.0.0.1, so serve a bootstrap
  # page instead: opening it fetches the trace same-origin and hands it to the
  # UI via postMessage. Unlike the ?url= flow below this survives an http
  # reverse proxy, so it needs no ssh port forwarding.
  bootstrap_html = None
  if remote_browser:
    bootstrap_html = _load_bootstrap_html(f'{origin}/#!/?{"&".join(params)}',
                                          fname)
    if bootstrap_html is None:
      print(f'Warning: {BOOTSTRAP_PATH} not found, falling back to 127.0.0.1')

  if bootstrap_html is None:
    bind_addr = '127.0.0.1'
    params.insert(0, f'url=http://127.0.0.1:{PORT}/{fname}')
    address = f'{origin}/#!/?{"&".join(params)}'
  else:
    bind_addr = '0.0.0.0'
    address = f'http://{socket.getfqdn()}:{PORT}/'

  os.chdir(os.path.dirname(path))
  socketserver.TCPServer.allow_reuse_address = True
  with socketserver.TCPServer((bind_addr, PORT), HttpHandler) as httpd:
    if open_browser and bootstrap_html is None:
      webbrowser.open_new_tab(address)
    else:
      print(f'Open URL in browser: {address}')

    httpd.expected_fname = fname
    httpd.fname_get_completed = None
    httpd.allow_origin = _strip_url_path(origin)
    httpd.bootstrap_html = bootstrap_html
    while httpd.fname_get_completed is None:
      httpd.handle_request()
