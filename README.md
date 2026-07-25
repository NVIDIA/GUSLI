<!--
SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->
# Gusli
[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![Release](https://img.shields.io/badge/Release-v0.04-brightgreen)](./CHANGELOG.md)
![Branch](https://img.shields.io/badge/origin/main-brightgreen)
<p align="center">
  <img src="./93docs/logo.png" alt="" width="400" />
</p>

## Description
Gusli (G3+ User Space Access Library) is a C++ library encapsulating IO (Read/Write) to any local block device (or a file).
It has a client library (.so) for IO submitting apps and server library (.so) for execution of the io's via backend
- Both client and server processes run on the same machine in user space. No Linux Kernel components involved.
- Zero copy kernel bypass io, using shared memory between processes
- 1 client connects to multiple servers. Each server represents a block device.
- Supported servers: SPDK based apps with standard bdevs, Any other process based on server library examples.
- IO (read/Write) can be blocking or asynchronous. Async-io can use callback functions and/or be polled for completion and/or canceled.
- The reason for separation of client and server is to avoid compile/link dependency between io submitting app and block device backend. Example: Backend might be spdk block device but client application does not have to be spdk application.
- Client library ensures io is valid & properly throttled before issuing it to the server

Additional Documentation and description: [Here](https://docs.google.com/document/d/1xXLyA2Di2G04zfLy8dzMor9DC2PNqUuXhMEFA2ZYpO4)

## Installation & Usage
`make all`
- For production `make clean all BUILD_RELEASE=1 BUILD_FOR_UNITEST=0 VERBOSE=1 ALLOW_USE_URING=0; ls /usr/lib/libg*; ls /usr/include/gus*;`
- executables of unit-tests will be created in project root dir (same as makefile)
- use the command above to build and install production libraries into /usr  (/usr/lib, /usr/include)
- make file does installation of binaries, see its output:
    - Deliverables: **.so/.hpp** files pair for server side and .so/.hpp files pair for client
- More info: type `make help` To see more info of how to compile
- Compilation: Both g++ and clang++ are supported. clang example: `make USE_CLANG=1 all`

### Dependencies
- Mandatory: None. Plain C++ code (libc) code.
- Third party components are not auto downloaded/installed. Please do the following:

| OS      |            Install                      |   Remarks      |
|---------|-----------------------------------------|----------------|
| Ubuntu  | `apt install g++ make cmake pkg-config` | ✅             |
| Fedora  | `dnf install gcc-c++ cmake pkg-config`  | 🚧 Not tested  |

- Optional:
    - For IO's with large scatter gathers (> 64 ranges), uring API to local block devices may help. So consider: `sudo apt install liburing-dev`
    - For developers: useful utils: `sudo apt install -y build-essential gdb meld ncdu tree valgrind`
- You can see the actual dependencies of this dynamic library by doing `ldd /usr/lib/libgusli_*.so`

### Monitor IO environment using Gusli
Bash script which allows you to monitor the io buffers, stacks, threads, etc of both client and server processes, as well as force kill them.
1. `source gusli/80scripts/service.sh`;
2. `GUSLI;` To get help about about monitoring client and server

### Examples (C++):
1. Client: [write-read-verify io](./07examples/client/sample_client.hpp)
    - function which connects to Gusli server issues some basic ios to it
    - `class unitest_io` shows how io can be launched in many modes (with/without async callback, with/without polling, blocking mode), with verification.
2. Server: [A few server classes](./07examples/server/) like ram based block device, always-fail-io, spdk-bdev, etc
3. Testing executables:
    - SPDK Both: Executable which forks itself, runs spdk server in 1 process, client basic io tests in another process [spdk_app](./unitest/sample_app_spdk_both.cpp)
    - `./unitest/run` for running various Gusli unit-tests
4. Gusli clients connect to servers via a config file. Example of config code can be found in a function `client_simple_test_of_server()`
5. See unit-test directory to learn more how to use the library and how it is tests.
6. How to link? See examples above

#### How to integrate with Gusli
1. First decide what does your app need. Serve io (like user space block device) or Issue io (then you are a client).
2. Use the supplied examples above to start from sample server/client application and extend as needed.
3. Application can be both server and client, this works but not recommended. See examples of self forking app.
4. If your app is a issuing io to standard linux block devices, then you can use only Gusli client, without server.
5. If your app is spdk based block device do the following steps: Download spdk, and install it. Compile Gusli example server app which loads Gusli server. Change spdk configuration file to match your block device
6. If your application is for production, please make sure you have a monitoring service/script which coordinates client/server apps
    - Generates proper configuration which block devices should be visible to client via servers
    - Restart server, client after crash
    - Note: Gusli is just 2 libraries, not services.

 #### Linkage type
- We strongly suggest to use **dynamic linking** to be able to enjoy latest Gusli features without recompiling your executable.
- Static linking is generally used for debugging and unit-testing only.
- You can use static linking (server/client) *.a files or dynamic linking *.so files.

### Integrations with other open source projects
- Integrating client with [NIXL](https://github.com/ai-dynamo/nixl), see [install script](./80scripts/integration_with_nixl.sh)
- Integrating server with [SPDK](https://github.com/spdk/spdk), see [install script](./80scripts/integration_with_spdkbdev.sh)

> ⚠️ Gusli team assisted with the above integrations for testing purposes. It is not responsible for it and integration can break if any of the above open source projects introduce breaking changes.

***
## Performance
### Concurrency
- Gusli Server is single threaded (1 cpu core for each block device). This is by design as multiple block devices may be used.
    - This thread receives client io, submits it to backend and sends completions to client
- Gusli client allocates 1 thread for for each block device.
    - This thead submits io to server and listens for completions from server
- You can submit and wait for IO from multiple threads. For more info read code documentation of client api header file

### Numbers
- Client is able to push about 1.5 - 2.5 Million ios per second to block device on the server
    - Meaning: 1 cpu 100% working on server and 1 cpu 100% on client
- If each io is 1[MB] this is equals to > 1 Terabyte per second.
- GUSLI cpu usage should not the bottle neck, Bottleneck will be other components (like physical driver of NVME or underlying disk bandwidth, network bandwidth etc)

***

## Support / Contributing
The current Maintainers Group for the project consists of:

| Maintainer              | GitHub ID                                             | Affiliation | Email               |
|-------------------------|-------------------------------------------------------|-------------|---------------------|
| Daniel Herman Shmulyan  | [danielhe](https://github.com/danielhe-nvidia)        | NVIDIA      | DanielHe@nvidia.com |


### Roadmap & Project status
1. [Jira Plan](https://jirasw.nvidia.com/browse/NVMESH-5811), accessible only internally in NVIDIA
3. Todos in the code are marked with `nvTODO()` macro

> [!Important]
> Carefully read the below known issues

#### Large known issues ⚠️
1. **Proper client server disconnect-reconnect while io is in air**. Examples:
    - Unmap/Remap mem buffers destroys io buffers! Think how to solve it
    - Client disconnect while Server backend returning completions - don't free io and executor
    - Properly handling ghost remaining ios after cancel or disconnect
    - This harms server upgradability. Server restarting while IO is in air will not resubmit the ios automatically.
2. **Proper Design for IO canceling**.
    - For now IO canceling is implemented as cancel and blocking wait for all after effects of io to finish. This can lead to unbounded wait. We need to better understand how users will use the cancel and design appropriately
    - Possible change: Don't block on canceled io but block on stop_all_ios() or unregister memory which those ios use.
3. **Proper Design for IO done()**. I introduced this destructor to force the user to actually think how they use the io api concurrently as io may have multithreaded access (completion callback, polling thread, io cancel thread, io destructor call). Explicit call to done() requires the user to understand when his io is terminated. But this is not mandatory and can be taken care internally in the io implementation via atomic refcount + block the ~destructor(). This however has its own costs, races and requires a careful planning
4. **Dynamic addition of servers** not supported. When starting Gusli library a full configuration of servers is given. Only subset of servers can be actually connected, but a new server which is not in the config, cannot be accessed by the client.
5. **Timeouts not support yet**.
    - User can monitor the IO and call cancel, but this does not guarantee io finish, much like stuck io to a local disk.
    - Control path operations (open/close bdev, etc) have no timeout and cannot be canceled. They are typically fast.
6. **IO throttling**. Done by the client on the amount of in air IO requests min(client_limit, server_limit). But does not take into account the size of the request nor how many ranges it has.
7. **Security issues**.
    - See [Security model and open issues, AI generated](./SECURITY.md)
    - See [AI agent security triage instruction](.security-triage.yaml)

### Version release checklist
1. Run Correctness tests (pass the following tests): `./unitest/run ci`;
2. Verify io performance did not suffer > 1.5[Miops]. Typically 2[M]+ for single core server/client
3. Verify no breaking changes introduces in external library API's
4. Tag the version. Example: `git tag -a v0.04 -m "changes a, b, fix c"; git push --tags; git ls-remote --tags origin`
5. Update the [change log](./CHANGELOG.md)

### Github Todos:
- [ ] Set up project integrations, Create a new merge request
- [ ] Automatically close issues from merge requests
- [ ] Enable merge request approvals
- [ ] SetUp CI/CD pipeline, Set automerge after approval and pipeline succeeds
- [ ] Analyze code for known vulnerabilities with Static Application Security Testing
- [ ] Deploy to Kubernetes, instead of just local makefile

***

