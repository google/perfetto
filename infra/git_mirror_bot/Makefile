# Copyright (C) 2017 The Android Open Source Project
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

PROJECT="perfetto-ci"
VM_NAME="perfetto-ci-git-mirror-bot"
ZONE="us-central1-c"

ssh:
	gcloud --account $(USER)@google.com \
		compute --project $(PROJECT) ssh --zone $(ZONE) gitbot@$(VM_NAME)

stop:
	gcloud compute \
		--project $(PROJECT) \
		instances delete $(VM_NAME) \
		--zone $(ZONE)

deploy_key:
	@echo "Download the deploy_key from the teams drive (go/perfetto_deploy_key)"
	@exit 1

start: deploy_key
	gcloud compute \
		--project $(PROJECT) \
		instances create $(VM_NAME) \
		--zone $(ZONE) \
		--machine-type "f1-micro" \
		--subnet "default" \
		--maintenance-policy "MIGRATE" \
		--image "debian-9-stretch-v20170918" \
		--image-project "debian-cloud" \
		--boot-disk-size "10" \
		--boot-disk-type "pd-standard" \
		--boot-disk-device-name "perfetto-ci-git-mirror-bot" \
		--metadata-from-file=startup-script=startup-script.sh,deploy_key=deploy_key,main=mirror_aosp_to_ghub_repo.py

.PHONY: ssh stop start
