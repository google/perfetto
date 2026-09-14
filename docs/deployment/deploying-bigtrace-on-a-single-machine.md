# Deploy BigTrace on a single machine

NOTE: This page is for administrators of Bigtrace services, not Bigtrace users.
Googlers should see `go/bigtrace` instead.

There are multiple ways to deploy Bigtrace on a single machine:

1. Running the Orchestrator and Worker executables manually
2. docker-compose
3. minikube

NOTE: Options 1 and 2 are intended for development and are not recommended for
production. For production, follow the instructions on
[Deploying Bigtrace on Kubernetes.](deploying-bigtrace-on-kubernetes)

## Prerequisites
To build Bigtrace you must first follow the
[Quickstart setup and building](/docs/contributing/getting-started.md#quickstart)
steps, but use `tools/install-build-deps --grpc` to install the required
dependencies for Bigtrace and gRPC.

## Running the Orchestrator and Worker executables manually
Build the executables before running them locally:

### Building the Orchestrator and Worker executables
```bash
tools/ninja -C out/[BUILD] orchestrator_main
tools/ninja -C out/[BUILD] worker_main
```

### Running the Orchestrator and Worker executables
Run the Orchestrator and Worker executables using command-line arguments:

```bash
./out/[BUILD]/orchestrator_main [args]
./out/[BUILD]/worker_main [args]
```

### Example
This example creates a service with an Orchestrator and three Workers. You can
interact with it locally using the Python API.
```bash
tools/ninja -C out/linux_clang_release orchestrator_main
tools/ninja -C out/linux_clang_release worker_main

./out/linux_clang_release/orchestrator_main -w "127.0.0.1" -p "5052" -n "3"
./out/linux_clang_release/worker_main --socket="127.0.0.1:5052"
./out/linux_clang_release/worker_main --socket="127.0.0.1:5053"
./out/linux_clang_release/worker_main --socket="127.0.0.1:5054"
```

## docker-compose
Use docker-compose to test gRPC without the overhead of Kubernetes. It builds
the Dockerfiles specified in infra/bigtrace/docker and creates containerized
instances of the Orchestrator and the specified set of Worker replicas.

```bash
cd infra/bigtrace/docker
docker compose up
# OR if using the docker compose standalone binary
docker-compose up
```
This will build and start the Workers (default of 3) and Orchestrator as specified in the `compose.yaml`.

## minikube
Use a minikube cluster to emulate the Kubernetes cluster setup on a local
machine. Create it with the script `tools/setup_minikube_cluster.sh`.

This starts a minikube cluster, builds the Orchestrator and Worker images and
deploys them on the cluster. You can then connect through a client such as the
Python API, using `minikube ip:30051` as the Orchestrator service address.

