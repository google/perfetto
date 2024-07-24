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

.EXPORT_ALL_VARIABLES:
GOOGLE_CLOUD_PROJECT=${PROJECT}

test: config.py common_utils.py
	gcloud auth application-default login --impersonate-service-account perfetto-ci@appspot.gserviceaccount.com
	python3 `which dev_appserver.py` app.yaml --support_datastore_emulator=False --log_level ${LOGLEVEL}

deploy: config.py common_utils.py
	gcloud app deploy -q app.yaml cron.yaml \
		--project ${PROJECT} \
		-v ${GAE_VERSION} \
		--stop-previous-version

stop:
		gcloud app instances delete \
		$(shell gcloud app instances list --project ${PROJECT} -v ${GAE_VERSION} -s default | tail -n1 | awk '{print $$3}') \
		--project ${PROJECT} -v ${GAE_VERSION} -s default -q

config.py: ../config.py
	cp ../$@ $@

common_utils.py: ../common_utils.py
	cp ../$@ $@

.PHONY: deploy test
