#!/usr/bin/env python3

import sys
import argparse
import urllib
import json
import urllib.request
import urllib.parse
import ssl
import hashlib
import copy

BUCKET_NAME = "perfetto-ui-data"
CURRENT_STATE_VERSION = 28


def upgrade_15(old):
  new = copy.deepcopy(old)
  new["version"] = 16
  new["flamegraphModalDismissed"] = False
  return new


def upgrade_16(old):
  new = copy.deepcopy(old)
  new["version"] = 17
  new["nextId"] = max(old["nextId"], old["nextNoteId"], old["nextAreaId"])
  engines = old["engines"]
  if len(engines) > 0:
    new["currentEngineId"] = list(engines.values())[0]['id']
  return new


def upgrade_17(old):
  new = copy.deepcopy(old)
  new["version"] = 18
  # TODO(hjd): Update
  return new


def upgrade_18(old):
  new = copy.deepcopy(old)
  new["version"] = 19
  # TODO(hjd): Update
  return new


def upgrade_19(old):
  new = copy.deepcopy(old)
  new["version"] = 20
  # TODO(hjd): Update
  return new


def upgrade_20(old):
  new = copy.deepcopy(old)
  new["version"] = 21
  # TODO(hjd): Update
  return new


def upgrade_20(old):
  new = copy.deepcopy(old)
  new["version"] = 22
  # TODO(hjd): Update
  return new


def upgrade_21(old):
  new = copy.deepcopy(old)
  new["version"] = 22
  # TODO(hjd): Update
  return new


def upgrade_22(old):
  new = copy.deepcopy(old)
  new["version"] = 23
  new["logFilteringCriteria"] = {
      "minimumLevel": 2,
  }
  return new


def upgrade_23(old):
  new = copy.deepcopy(old)
  new["version"] = 24
  current_engine_id = new["currentEngineId"]
  new["engine"] = new["engines"][
      current_engine_id] if current_engine_id else None
  del new["currentEngineId"]
  del new["engines"]
  return new


def upgrade_24(old):
  new = copy.deepcopy(old)
  new["version"] = 25
  new["omniboxState"] = new["frontendLocalState"]["omniboxState"]
  del new["frontendLocalState"]["omniboxState"]
  return new


def upgrade_25(old):
  new = copy.deepcopy(old)
  new["version"] = 26
  new["logFilteringCriteria"]["tags"] = []
  return new


def upgrade_26(old):
  new = copy.deepcopy(old)
  new["version"] = 27
  new["logFilteringCriteria"]["textEntry"] = ""
  return new


def upgrade_27(old):
  new = copy.deepcopy(old)
  new["version"] = 28
  new["logFilteringCriteria"]["hideNonMatching"] = False
  return new


def bug_compatible_hash_mangling(hash):
  pairs = [hash[i:i + 2] for i in range(0, len(hash), 2)]
  return ''.join([pair.removeprefix("0") for pair in pairs])


def upload_state(state):
  data = state.encode("utf-8")
  hash = bug_compatible_hash_mangling(hashlib.sha256(data).hexdigest())

  try:
    get(make_state_url(hash))
  except:
    pass
  else:
    return hash

  url = f"https://www.googleapis.com/upload/storage/v1/b/{BUCKET_NAME}/o?uploadType=media&name={hash}&predefinedAcl=publicRead"
  request = urllib.request.Request(url, data=data)
  request.add_header("Content-Type", "application/json; charset=utf-8")
  response = urllib.request.urlopen(request)
  return hash


def make_state_url(id):
  return f"https://storage.googleapis.com/{BUCKET_NAME}/{id}"


def make_ui_url(id):
  return f"https://ui.perfetto.dev/#!/?s={id}"


def extract_state_uuid(url):
  fragment = urllib.parse.urlparse(url).fragment
  fragment = fragment.removeprefix("!/?")
  return urllib.parse.parse_qs(fragment)["s"][0]


def get(url):
  context = ssl._create_unverified_context()
  response = urllib.request.urlopen(url, context=context)
  contents = response.read().decode()
  return contents


def post(url):
  context = ssl._create_unverified_context()
  response = urllib.request.urlopen(url, context=context)
  contents = response.read().decode()
  return contents


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument("permalink", help="Permalink you wish to update")
  parser.add_argument(
      "--target-version",
      help=f"Target state version (default: {CURRENT_STATE_VERSION})",
      default=CURRENT_STATE_VERSION)
  parser.add_argument(
      "--verbose", help=f"Show debug information", action="store_true")
  args = parser.parse_args()

  permalink_url = args.permalink
  old_uuid = extract_state_uuid(permalink_url)
  old_state_url = make_state_url(old_uuid)
  old_state = get(old_state_url)
  old_json = json.loads(old_state)

  old_state_version = old_json["version"]
  new_state_version = args.target_version

  print(json.dumps(old_json, sort_keys=True, indent=4))

  UPGRADE = {
      15: upgrade_15,
      16: upgrade_16,
      17: upgrade_17,
      18: upgrade_18,
      19: upgrade_19,
      20: upgrade_20,
      21: upgrade_21,
      22: upgrade_22,
      23: upgrade_23,
      24: upgrade_24,
      25: upgrade_25,
      26: upgrade_26,
      27: upgrade_27,
  }

  new_json = old_json
  for i in range(old_state_version, new_state_version):
    new_json = UPGRADE[i](new_json)

  new_state = json.dumps(new_json)
  new_uuid = upload_state(new_state)
  new_url = make_ui_url(new_uuid)
  print(f"Your new permalink is accessible at:")
  print(f"{new_url}")
  return 0


if __name__ == "__main__":
  sys.exit(main())
