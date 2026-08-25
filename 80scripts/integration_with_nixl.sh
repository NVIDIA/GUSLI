#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) <year> NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Tested on ubuntu and ubuntu WSL in Windows11. Integration with NIXL library, Jira ticket: NVMESH-5239
# Step 1.0: Install docker and basic dependencies
sudo apt update
sudo apt install -y apt-transport-https ca-certificates curl software-properties-common gnupg lsb-release
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo apt-key add -
apt-cache policy docker-ce;
sudo apt install docker-ce;
sudo systemctl status docker;

# Step 1.1: Clone NIXL source
cd ~/projects;
git clone ssh://git@github.com/ai-dynamo/nixl.git;

# Step 2: Create NIXL container
cd ~/projects/nixl; sudo ./contrib/build-container.sh
sudo docker images; # See which image we have, example
	# REPOSITORY   TAG                  IMAGE ID       CREATED        SIZE
	# nixl         v0.1.0.dev.fa90267   ad35c624545f   27 hours ago   22.3GB
	# nixl         v0.1.0.dev.f1464c3   5de048889167   3 weeks ago    19.1GB
if false; then
	# Unsecure docker, supports lib-iouring io
	sudo docker run --security-opt seccomp=unconfined -v /home/danielhe/projects/nixl:/home/danielhe/projects/nixl:Z -it b8a0841505f3 /bin/bash
else
	sudo docker run --shm-size=5g -v /home/danielhe/projects/nixl:/home/danielhe/projects/nixl:Z -it b8a0841505f3 /bin/bash
fi;

################################ Do inside container
# Step 3.0: Install generic stuff
apt-get update; apt-get install -y libgflags-dev meson autoconf libtool gdb htop libgtest-dev liburing-dev libaio-dev clangd tree xxd sudo;

# Step 3.1: Install latest gusli lib
cd /home/danielhe/projects/nixl;
if false; then								# Debugging / Developing GUSLI-NIXL integration
	rm -rf ./gusli; cp -r ../gusli .;		# Copy local dev version
else
	rm -rf ./gusli; git clone https://github.com/NVIDIA/GUSLI.git;		# Public repo
	# git clone git@github.com:nvidia/gusli.git; 		# Private repo
	# git clone ssh://git@github.com/nvidia/gusli.git;	# Private repo
fi;
cd gusli && make clean all BUILD_RELEASE=1 BUILD_FOR_UNITEST=0 VERBOSE=1 ALLOW_USE_URING=0 TRACE_LEVEL=5 && cd .. && ls /usr/lib/libg* && ls /usr/include/gus*;
source gusli/80scripts/service.sh;

# Step 3.2: Build NIXL unitests
mkdir -p /root/NNN; mkdir -p /root/NNP; meson setup --reconfigure -Dbuildtype=debug -Dprefix=/root/NNP /root/NNN; cd /home/danielhe/projects/nixl;
clear; ninja -C /root/NNN install;

# Step 3.3: Run Gusli Plugin within NIXL unitest
clear; /root/NNN/test/unit/plugins/gusli/nixl_gusli_test; GUSLI show;
rm /root/NNN/meson-logs/testlog.txt; meson test gusli_plugin_test -C /root/NNN; cat /root/NNN/meson-logs/testlog.txt
for i in {1..50}; do  /root/NNN/test/unit/plugins/gusli/nixl_gusli_test 2>&1 | tee -a z_out.txt; done;

################################ If changed gusli plugin within NIXL, make sure it is clang formatted
apt install -y clang-format
clang-format -style=file -i src/plugins/gusli/*.cpp src/plugins/gusli/*.h test/unit/plugins/gusli/nixl_gusli_test.cpp

