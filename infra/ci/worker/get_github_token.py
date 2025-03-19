#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the 'License');
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an 'AS IS' BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
""" This script does all the token dance to register a GitHub action runner

- The script impersonates the Perfetto CI App GitHub App using the app private
  key.
- It then obtains the app installation token, which binds the app to the
  Perfetto GitHub repo.
- From that obtains the "Runner registration token".
- This token is then passed to the sandbox, so it can run the
  GitHub Action Runner (./config --unmanned --token=...)
"""

import jwt
import time
import requests
import subprocess

from config import PROJECT, GITHUB_REPO, GITHUB_APP_ID, GITHUB_APP_INSTALLATION_ID

GITHUB_API_URL = 'https://api.github.com'


def generate_jwt():
  private_key = subprocess.check_output([
      'gcloud', '--project', PROJECT, 'secrets', 'versions', 'access', 'latest',
      '--secret=perfetto_ci_github_private_key'
  ]).decode()

  now = int(time.time())
  payload = {
      'iat': now,
      'exp': now + (10 * 60),  # JWT valid for 10 minutes
      'iss': GITHUB_APP_ID
  }
  return jwt.encode(payload, private_key, algorithm='RS256')


def get_installation_token(jwt_token, installation_id):
  url = f'{GITHUB_API_URL}/app/installations/{installation_id}/access_tokens'
  headers = {
      'Authorization': f'Bearer {jwt_token}',
      'Accept': 'application/vnd.github.v3+json'
  }
  response = requests.post(url, headers=headers)
  response.raise_for_status()
  return response.json()['token']


def get_runner_registration_token(inst_token):
  url = f'{GITHUB_API_URL}/repos/{GITHUB_REPO}/actions/runners/registration-token'
  headers = {
      'Authorization': f'token {inst_token}',
      'Accept': 'application/vnd.github.v3+json'
  }
  response = requests.post(url, headers=headers)
  response.raise_for_status()
  return response.json()['token']


def get_github_token():
  jwt_token = generate_jwt()
  inst_token = get_installation_token(jwt_token, GITHUB_APP_INSTALLATION_ID)
  registration_token = get_runner_registration_token(inst_token)
  return registration_token


if __name__ == '__main__':
  print(get_github_token)
