# Perfetto CI

This CI is used on-top of (not in replacement of) Android's TreeHugger.
It gives early testing signals and coverage on other OSes and older Android
devices not supported by TreeHugger.

See the [Testing](/docs/contributing/testing.md) page for more details about the
project testing strategy.

The CI is based on GitHub actions.
The entry-point is [.github/workflows/analyze.yml](/.github/workflows/analyze.yml).

The analyze step triggers other workflows (UI, linux tests, etc) depending on
the changed files.

## Self-hosted runners in GCE

We use self-hosted runners that register on the GitHub project.

The Google Cloud project that hosts them is called `perfetto-ci`

The source code lives in [infra/ci](/infra/ci)

## Worker GCE VMs

We have a variable number of GCE vms (see `GCE_VM_TYPE` in config.py) driven by
an autoscaler, up to `MAX_VMS_PER_REGION`.

Each GCE vm runs a fixed number (`NUM_WORKERS_PER_VM`) of `sandbox` containers.

Each sandbox container runs an instance of a GitHub Action Runner.

The build and test happens within the Action Runner.

On top of this, each GCE vm runs one privileged Docker container called
`worker`. The worker handles the basic setup of the VM and does nothing other
than ensuring that there are always N sandboxes running, via supervisord.

The whole system image is read-only. The VM itself is stateless. No state is
persisted outside of Google Cloud Storage (only for UI artifacts) and GitHub's
cache. The SSD is used only as a scratch disk for swap - to use a large tmpfs -
and is cleared on each reboot.

VMs are dynamically spawned using the Google Cloud Autoscaler and use a
Stackdriver Custom Metric pushed by the ci.perfetto.dev AppEngine as cost
function. Such metric is the number of queued + running pull-requests.

The GCE vm and the privileged docker container run with the service account
`gce-ci-worker@perfetto-ci.iam.gserviceaccount.com`.

The sandbox runs with a restricted service account
`gce-ci-sandbox@perfetto-ci.iam.gserviceaccount.com` which is only allowed to
create - but not delete or overwrite - artifacts in gs://perfetto-ci-artifacts

# Sequence Diagram

This is what happens, in order, on a worker instance from boot to the test run.

```bash
make -C /infra/ci worker-start
┗━ gcloud start ...

[GCE] # From /infra/ci/worker/gce-startup-script.sh
docker run worker ...

[worker] # From /infra/ci/worker/Dockerfile
┗━ /infra/ci/worker/worker_entrypoint.sh
  ┗━ supervisord
    ┗━ [N] /infra/ci/worker/sandbox_runner.py
      ┗━ docker run sandbox-N ...

[sandbox-X] # From /infra/ci/sandbox/Dockerfile
┗━ /infra/ci/sandbox/sandbox_entrypoint.sh
  ┗━ github-action-runner/run.sh
    ┗━ .github/workflows/analyze.yml
      ┣━ .github/workflows/linux-tests.yml
      ┣━ .github/workflows/ui-tests.yml
         ...
      ┗━ .github/workflows/android-tests.yml
```

## Playbook

### Frontend (JS/HTML/CSS/py) changes

Test-locally: `make -C infra/ci/frontend test`

Deploy with `make -C infra/ci/frontend deploy`

### Worker/Sandbox changes

1. Build and push the new docker containers with:

   `make -C infra/ci build push`

2. Restart the GCE instances, either manually or via

   `make -C infra/ci restart-workers`

## Security considerations

- The gs://perfetto-artifacts GCS bucket are world-readable and writable by
  the GAE and GCE service accounts.

- Overall, no account in this project has any interesting privilege:
  - The worker and sandbox service accounts don't have any special capabilities
    outside of the CI project itself. Even if compromised they wouldn't allow
    anything that couldn't be done by spinning your own Google Cloud project.

- This CI deals only with functional and performance testing and doesn't deal
  with any sort of continuous deployment.

- GitHub actions are triggered automatically only for perfetto-team and
  perfetto-contributors.

- Sandboxes are not too hard to escape (Docker is the only boundary).

- As such neither pre-submit nor post-submit build artifacts are considered
  trusted. They are only used for establishing functional correctness and
  performance regression testing.

- Binaries built by the CI are not ran on any other machines outside of the
  CI project. They are deliberately not pushed to GCS.

- The only build artifacts that are retained (for up to 30 days) and uploaded to
  the GCS bucket are the UI artifacts. This is for the only sake of getting
  visual previews of the HTML changes.

- UI artifacts are served from a different origin (the GCS per-bucket API) than
  the production UI.
