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

test: LOGLEVEL=info
test: static_3p config.py common_utils.py static/config.js
	python3 `which dev_appserver.py` app.yaml --dev_appserver_log_level ${LOGLEVEL}

deploy: static_3p config.py common_utils.py static/config.js
	gcloud app deploy -q app.yaml \
		--project ${PROJECT} \
		-v ${GAE_VERSION} \
		--stop-previous-version

config.py: ../config.py
	cp ../$@ $@

common_utils.py: ../common_utils.py
	cp ../$@ $@

static/config.js: ../config.py
	../config.py js > $@

static/third_party/xterm-4.11.0.min.css:
	curl -Sso $@ https://unpkg.com/xterm@4.11.0/css/xterm.css
	echo "4cee0ccb82803888905fefc8641f0613c0a9081b  $@" | ${SHASUM} -c || rm $@

static/third_party/xterm-4.11.0.min.js:
	curl -Sso $@ https://unpkg.com/xterm@4.11.0/lib/xterm.js
	echo "470644a5cd3e2a443a422c6ecddc85b37a85f329  $@" | ${SHASUM} -c || rm $@

static/third_party/xterm-0.8.0-addon-search.min.js:
	curl -Sso $@ https://unpkg.com/xterm-addon-search@0.8.0/lib/xterm-addon-search.js
	echo "1eae1371cc07677c38f9d01eae4d6a3b996388f8  $@" | ${SHASUM} -c || rm $@

static/third_party/xterm-0.5.0-addon-fit.min.js:
	curl -Sso $@ https://unpkg.com/xterm-addon-fit@0.5.0/lib/xterm-addon-fit.js
	echo "4c8942910554242336443126e349ac1e3897a64e  $@" | ${SHASUM} -c || rm $@

static/third_party/mithril-1.1.6.min.js:
	curl -Sso $@ https://cdnjs.cloudflare.com/ajax/libs/mithril/1.1.6/mithril.min.js
	echo "a204c02ee15c347cf368c3481bdea967b443c8d0  $@" | ${SHASUM} -c || rm $@

static_3p: static/third_party/mithril-1.1.6.min.js static/third_party/xterm-4.11.0.min.css static/third_party/xterm-4.11.0.min.js static/third_party/xterm-0.8.0-addon-search.min.js static/third_party/xterm-0.5.0-addon-fit.min.js

.PHONY: test deploy static_3p
