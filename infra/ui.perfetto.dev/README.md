# ui.perfetto.dev Cloud scripts

See [go/perfetto-ui-autopush](http://go/perfetto-ui-autopush) for docs on how
this works end-to-end.

## Channel deployment model

Three channels are served from `gs://ui.perfetto.dev/`:

| Channel  | Source branch | Trigger                    |
|----------|---------------|----------------------------|
| autopush | `main`        | push to `main`             |
| canary   | `canary`      | push to `canary`           |
| stable   | `stable`      | push to `stable`           |

Each Cloud Build trigger invokes `ui_builder_entrypoint.sh` with the
channel name as `$1`. The entrypoint runs `ui/release/build_channel.py
--channel=<name> --upload`, which builds the UI from the branch HEAD
and uploads `/v<version>/**` to GCS.

The shared root `/index.html` (and `/bigtrace.html`) carries a
`data-perfetto_version='{"stable":...,"canary":...,"autopush":...}'` map
that the UI bootstrap reads to decide which `/v<version>/` to load. To
keep parallel deploys race-free, each channel only modifies its own entry
in that map, using GCS `x-goog-if-generation-match` as a CAS primitive.

The stable channel additionally owns the HTML body itself (it is the
only channel that overwrites the body) and the shared `/service_worker.*`
files. Canary and autopush never write the body or service_worker, so
canary instability cannot break stable users.

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
    /ui_builder_entrypoint.sh <channel>
```

where `<channel>` is one of `autopush`, `canary`, `stable` (see the
channel table above).

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
