# Deploying Bigtrace one a single machine

NOTE: This doc is designed for administrators of Bigtrace services NOT Bigtrace users. This is also designed for non-Googlers - Googlers should look at `go/bigtrace` instead.

There are multiple ways to deploy Bigtrace on a single machine:

1. Running the Orchestrator and Worker executables manually
2. docker-compose
3. minikube

NOTE: Options 1 and 2 are intended for development purposes and are not recommended for production. For production purposes instead follow the instructions on [Deploying Bigtrace on Kubernetes.](deploying-bigtrace-on-kubernetes)

## Prerequisites
To build Bigtrace you must first follow the [Quickstart setup and building](/docs/contributing/getting-started.md#quickstart) steps but using `tools/install-build-deps --grpc` in order to install the required dependencies for Bigtrace and gRPC.

## Running the Orchestrator and Worker executables manually
To manually run Bigtrace locally with the executables you must first build the executables before running them as follows:

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
Creates a service with an Orchestrator and three Workers which can be interacted with using the Python API locally.
```bash
tools/ninja -C out/linux_clang_release orchestrator_main
tools/ninja -C out/linux_clang_release worker_main

./out/linux_clang_release/orchestrator_main -w "127.0.0.1" -p "5052" -n "3"
./out/linux_clang_release/worker_main --socket="127.0.0.1:5052"
./out/linux_clang_release/worker_main --socket="127.0.0.1:5053"
./out/linux_clang_release/worker_main --socket="127.0.0.1:5054"
```

## docker-compose
To allow testing of gRPC without the overhead of Kubernetes, docker-compose can be used which builds the Dockerfiles specified in infra/bigtrace/docker and creates containerised instances of the Orchestrator and the specified set of Worker replicas.

```bash
cd infra/bigtrace/docker
docker compose up
# OR if using the docker compose standalone binary
docker-compose up
```
This will build and start the Workers (default of 3) and Orchestrator as specified in the `compose.yaml`.

## minikube
A minikube cluster can be used to emulate the Kubernetes cluster setup on a local machine. This can be created with the script `tools/setup_minikube_cluster.sh`.

This starts a minikube cluster, builds the Orchestrator and Worker images and deploys them on the cluster. This can then be interacted with using the `minikube ip`:5051 as the Orchestrator service address through a client such as the Python API.

