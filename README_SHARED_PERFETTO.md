## Shared Perfetto Web UI
### Build
- Update dependencies
```
tools/install-build-deps
tools/install-build-deps --ui
```
- build ui
```
ui/build
ui/run-dev-server
```

- build processor
```
tools/gn args out/ubuntu
tools/ninja -C out/ubuntu trace_processor_shell
```

Build args
```
> vim out/ubuntu/args.gn
target_os = "linux"
target_cpu = "x64"
```

### Example
Run trace_processor
```
./out/ubuntu/trace_processor_shell --httpd
```

Open: http://0.0.0.0:10000/

To open a stored file that has been uploaded to the server:
http://0.0.0.0:10000/#!/viewer?storage=fileName

Or open the "Upl0oaded Files" page in the sidebar.

To build a docker image: 
docker build -f Docker/Dockerfile .

### Design Document
- https://docs.google.com/document/d/16ylk61fJBYUsW6SEbCLTnGeYAqpFhptJmmyQ0eEElcw/edit?usp=sharing