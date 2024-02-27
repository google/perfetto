# Copyright (C) 2021 The Android Open Source Project
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

import flask
import requests

BUCKET_NAME = 'ui.perfetto.dev'

REQ_HEADERS = [
    'Accept',
    # TODO(primiano): re-enable once the gzip handling outage fixed.
    # 'Accept-Encoding',
    # 'Cache-Control',
]

RESP_HEADERS = [
    'Content-Type',
    'Content-Encoding',
    'Content-Length',
    'Cache-Control',
    'Date',
    'Expires',
]

app = flask.Flask(__name__)


# Redirect v1.2.3 to v.1.2.3/
@app.route('/v<int:x>.<int:y>.<int:z>')
def version_redirect(x, y, z):
  return flask.redirect('/v%d.%d.%d/' % (x, y, z), code=302)


# Serve the requests from the GCS bucket.
@app.route('/', methods=['GET'])
@app.route('/<path:path>', methods=['GET'])
def main(path=''):
  path = '/' + path
  path += 'index.html' if path.endswith('/') else ''
  req_headers = {}
  for key in set(flask.request.headers.keys()).intersection(REQ_HEADERS):
    req_headers[key] = flask.request.headers.get(key)
  url = 'https://commondatastorage.googleapis.com/' + BUCKET_NAME + path
  req = requests.get(url, headers=req_headers)
  if (req.status_code != 200):
    return flask.abort(req.status_code)
  resp = flask.Response(req.content)
  for key in set(req.headers.keys()).intersection(RESP_HEADERS):
    resp.headers[key] = req.headers.get(key)
  return resp


if __name__ == '__main__':
  # This is used when running locally only.
  app.run(host='127.0.0.1', port=10000, debug=True)
