# ui.perfetto.dev Cloud scripts

See [go/perfetto-ui-autopush](http://go/perfetto-ui-autopush) for docs on how
this works end-to-end.

## /appengine : GAE <> GCS proxy

The Google AppEngine instance that responds to ui.perfetto.dev.
It simply passes through the requests to the bucket gs://ui.perfetto.dev .
This should NOT be re-deployed when uploading a new version of the ui,
as the actual UI artifacts live in GCS.

We are using AppEngine for historical reasons, at some point this should
be migrated to a Type 7 Google Cloud Load Balancer, which supports
direct backing by a GCS bucket. The only blocker for that is figuring out
a seamless migration strategy for the SSL certificate.

## /builder : Docker container for Google Cloud Build

Contains the Dockerfile to generate the container image which is used by
Google Cloud Build when auto-triggering new ui builds.
Cloud Build invokes the equivalent of:

```bash
docker run europe-docker.pkg.dev/perfetto-ui/builder/perfetto-ui-builder \
    /ui_builder_entrypoint.sh
```

NOTE: the `ui_builder_entrypoint.sh` script is bundled in the docker container.
The container needs to be re-built and re-pushed if the script changes.

To update the container:

Prerequisite:
Install the Google Cloud SDK from https://dl.google.com/dl/cloudsdk/release/google-cloud-sdk.tar.gz 


```bash
# Obtain a temporary token to impersonate the service account as per
# https://cloud.google.com/artifact-registry/docs/docker/authentication
# You need to be a member of perfetto-cloud-infra.prod to do this.
gcloud auth print-access-token \
    --impersonate-service-account perfetto-ui-dev@perfetto-ui.iam.gserviceaccount.com | docker login \
    -u oauth2accesstoken \
    --password-stdin https://europe-docker.pkg.dev

docker build -t europe-docker.pkg.dev/perfetto-ui/builder/perfetto-ui-builder infra/ui.perfetto.dev/builder
docker push europe-docker.pkg.dev/perfetto-ui/builder/perfetto-ui-builder
```
