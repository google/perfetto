# Copyright (C) 2019 The Android Open Source Project
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


include $(shell python3 ../config.py makefile)

SHASUM=shasum
GOOGLE_CLOUD_PROJECT=perfetto-ci

test: LOGLEVEL=info
test: static_3p config.py common_utils.py static/config.js
	gcloud auth application-default login \
			--impersonate-service-account perfetto-ci@appspot.gserviceaccount.com
	python3 `which dev_appserver.py` app.yaml \
		--dev_appserver_log_level ${LOGLEVEL} \
		--port 8081

deploy: static_3p config.py common_utils.py static/config.js
	gcloud app deploy -q app.yaml cron.yaml \
		--project ${PROJECT} \
		-v ${GAE_VERSION} \
		--stop-previous-version

config.py: ../config.py
	cp ../$@ $@

common_utils.py: ../common_utils.py
	cp ../$@ $@

static/config.js: ../config.py
	../config.py js > $@

static/third_party/mithril-1.1.6.min.js:
	curl -Sso $@ https://cdnjs.cloudflare.com/ajax/libs/mithril/1.1.6/mithril.min.js
	echo "a204c02ee15c347cf368c3481bdea967b443c8d0  $@" | ${SHASUM} -c || rm $@

static_3p: static/third_party/mithril-1.1.6.min.js

.PHONY: test deploy static_3p
