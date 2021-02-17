#!/usr/bin/env python3
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
import os

from google.cloud import storage

BUCKET_NAME = 'perfetto.dev'

app = flask.Flask(__name__)
bucket = None
is_local_testing_instance = False


@app.route('/_ah/<path:path>')
def ignore_app_engine_lifecycle(path):
  return flask.abort(404)


@app.route('/docs')
def docs_redirect():
  return flask.redirect('/docs/', code=301)


# Serve the requests from the GCS bucket.
@app.route('/', methods=['GET'])
@app.route('/<path:path>', methods=['GET'])
def main(path=''):
  # Force redirect HTTP -> HTTPS.
  if not flask.request.is_secure and not is_local_testing_instance:
    https_url = flask.request.url.replace('http://', 'https://', 1)
    return flask.redirect(https_url, code=301)
  if flask.request.host == 'www.perfetto.dev':
    return flask.redirect(
        flask.request.url.replace('www.perfetto.dev', 'perfetto.dev'))
  if flask.request.host == 'docs.perfetto.dev':
    return flask.redirect('https://perfetto.dev/docs/')

  path = '/' + path
  path += 'index.html' if path.endswith('/') else ''
  global bucket
  if bucket is None:
    bucket = storage.Client().get_bucket(BUCKET_NAME)
  blob = bucket.get_blob(path[1:])
  if blob is None:
    return flask.abort(404)
  data = blob.download_as_bytes()
  resp = flask.Response(data)
  resp.headers['Content-Type'] = blob.content_type
  resp.headers['Content-Length'] = len(data)
  resp.headers['Content-Encoding'] = blob.content_encoding
  if os.path.splitext(path)[1] in ('.png', '.svg'):
    resp.headers['Cache-Control'] = 'public, max-age=86400'  # 1 Day
  else:
    resp.headers['Cache-Control'] = 'public, max-age=600'  # 10 min
  return resp


def get_credentials_for_local_testing():
  from google_auth_oauthlib import flow
  flow = flow.InstalledAppFlow.from_client_config(
      client_config={
          'installed': {
              # These aren't secret. Copied from gsutil's apitools sources.
              'client_id': '1042881264118.apps.googleusercontent.com',
              'client_secret': 'x_Tw5K8nnjoRAqULM9PFAC2b',
              'redirect_uris': ['urn:ietf:wg:oauth:2.0:oob'],
              'auth_uri': 'https://accounts.google.com/o/oauth2/auth',
              'token_uri': 'https://accounts.google.com/o/oauth2/token'
          }
      },
      scopes=['https://www.googleapis.com/auth/devstorage.read_only'])
  creds = flow.run_console()
  return creds


if __name__ == '__main__':
  # This is used when running locally only.
  creds = get_credentials_for_local_testing()
  storage_client = storage.Client(project='perfetto-site', credentials=creds)
  bucket = storage_client.bucket(BUCKET_NAME)
  is_local_testing_instance = True
  app.run(host='127.0.0.1', port=8082, debug=False)
