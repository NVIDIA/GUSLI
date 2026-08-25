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
# ********** Define compiler and compilation flags *********
BUILD_RELEASE?=1
USE_CLANG?=0
BUILD_FOR_UNITEST?=1
ALLOW_USE_URING?=1
BUILD_ALSO_INSTALLS_DYN_LIBS?=1
UNAME := $(shell uname)# Which operating system we use?
HT = $(shell uname -m)
CFLAGS = $(EXTRA_CFLAGS)
ifeq ($(UNAME),Darwin)
    USE_DARWIN=1
    CFLAGS += -m64
    LIBS_PATH = -L/usr/lib
else ifeq ($(HT),x86_64)
    USE_64_BITS=1
    CFLAGS += -m64
    LIBS_PATH = -L/usr/lib64
else ifeq ($(HT),aarch64)
    USE_ARM=1
    CFLAGS += -march=armv8.1-a+crc+fp+simd
    LIBS_PATH = -L/usr/lib64
else
    $(error Unknown OS/Host)
endif

VERBOSE?=0
ifeq ($(VERBOSE), 1)
    ECHO_CMD =
else
    ECHO_CMD = @
endif

# ********** Included directories of H files *********
# Main project directory. (relative to this Makefile)
THIS_MKFILE_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
ROOT_DIR_RLTV := $(THIS_MKFILE_DIR)
ROOT_DIR_ABS = $(realpath $(ROOT_DIR_RLTV))
INCLUDES :=
INCLUDES += -I/usr/include -I. -I./00utils -I./01common -I./03backend -I./05clnt

# ********** *.cpp sources *********
SOURCES_BASE = 00utils/utils.cpp 01common/shared_mem_bufs.cpp
SOURCES_CLNT = 05clnt/client_imp.cpp
SOURCES_SRVR = 03backend/server_imp.cpp
SOURCES_TEST_CLNT = unitest/unitest.cpp
SOURCES_SPDK_SRVR_APP = unitest/sample_app_spdk_server.cpp
SOURCES_SPDK_BOTH_APP = unitest/sample_app_spdk_both.cpp
SOURCES_ALL = $(SOURCES_BASE) $(SOURCES_CLNT) $(SOURCES_SRVR) $(SOURCES_TEST_CLNT) $(SOURCES_SPDK_BOTH_APP) $(SOURCES_SPDK_SRVR_APP)

# ********** All objects will reside in separate directory **********
OBJ_DIR_ROOT=99bin
ifeq ($(BUILD_RELEASE),1)
    OBJ_DIR=./$(OBJ_DIR_ROOT)/release
else
    OBJ_DIR=./$(OBJ_DIR_ROOT)/debug
endif

$(shell mkdir -p $(OBJ_DIR))
OBJECTS_BASE = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES_BASE))
OBJECTS_CLNT = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES_CLNT))
OBJECTS_SRVR = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES_SRVR))
OBJECTS_TEST_CLNT = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES_TEST_CLNT))
OBJECTS_SPDK_BOTH_APP = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES_SPDK_BOTH_APP))
OBJECTS_SPDK_SRVR_APP = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES_SPDK_SRVR_APP))
OBJECTS_ALL = $(OBJECTS_BASE) $(OBJECTS_CLNT) $(OBJECTS_SRVR) $(OBJECTS_TEST_CLNT) $(OBJECTS_SPDK_BOTH_APP) $(OBJECTS_SPDK_SRVR_APP)

# Include dependency files if they exist, causes recompilation .cpp files when .hpp files change
DEPEND_FILES_ALL = $(OBJECTS_ALL:.o=.d)
DFLAGS = -MP -MMD

# Compilation output (Lib/Exe)
PROJECT_PREFIX=gusli
LIB_COMN_NAME=$(PROJECT_PREFIX)_comn
LIB_CLNT_NAME=$(PROJECT_PREFIX)_clnt
LIB_SRVR_NAME=$(PROJECT_PREFIX)_srvr
ifeq ($(BUILD_FOR_UNITEST),1)
    INSTALL_DIR=./$(OBJ_DIR_ROOT)/inst
else
    INSTALL_DIR=/usr
endif
UNITEST_EXE_PREFIX=z_
UNITEST_CLNT_EXE=$(UNITEST_EXE_PREFIX)$(LIB_CLNT_NAME)_unitest
UNITEST_SPDKS_EXE=$(UNITEST_EXE_PREFIX)$(PROJECT_PREFIX)_spdk_server_dyn
UNITEST_SPDK2_EXE=$(UNITEST_EXE_PREFIX)$(PROJECT_PREFIX)_spdk_both_unitest
EXE_LIST_ALL=$(UNITEST_CLNT_EXE) $(UNITEST_SPDK2_EXE) # Static executables
ifeq ($(BUILD_ALSO_INSTALLS_DYN_LIBS),1)
    # Install .so and generate dynamic link executables
    EXE_LIST_ALL += $(UNITEST_CLNT_EXE)_dyn $(UNITEST_SPDKS_EXE)
    TARGET_BINARIES_RULE = install $(EXE_LIST_ALL)
else
    TARGET_BINARIES_RULE = $(EXE_LIST_ALL)
endif

# ********** Extract git information *********
ifeq ($(COMPILATION_DATE),)
    ifeq ($(BUILD_FOR_UNITEST),1)
        COMPILATION_DATE := \!\!\!\!\ Non\ Production\ \!\!\!\!
        CFLAGS += -DUNITEST_ENV=1
    else
        COMPILATION_DATE := \($(shell date +%-d/%b/%Y:"%T")\)\[$(shell uname -r)\]
    endif
endif
ifeq ($(HAS_LOCAL_GIT),)
    HAS_LOCAL_GIT=$(shell git rev-parse --is-inside-work-tree)
endif
ifeq ($(HAS_LOCAL_GIT),true)
    COMMIT_ID=$(shell git log -n1 --format=%h)
    VER_TAGID=$(shell git describe --abbrev=0)
    BRANCH_NAME=$(shell git symbolic-ref 2>/dev/null --short --quiet HEAD || git rev-parse 2>/dev/null --abbrev-ref HEAD)
endif
# Dummy commit info
ifeq ($(COMMIT_ID),)
    COMMIT_ID := 010101010
    VER_TAGID := v3.1.0
    BRANCH_NAME := Unknown
endif

# ********** Define Linker flags *********
LFLAGS_ALL = -lpthread -rdynamic -lrt
LFLAGS_EXT =
# ********** External libraries ********* check for lib using pkg-config and update flags appropriately
ifeq ($(ALLOW_USE_URING),1)
    EXTLIB=liburing
    ifneq ($(shell pkg-config --exists $(EXTLIB) && echo yes),)
        CFLAGS += -DHAS_URING_LIB $(shell pkg-config --cflags $(EXTLIB))
        LFLAGS_EXT = $(shell pkg-config --libs $(EXTLIB))
    endif
endif

SPDK_DIR = $(ROOT_DIR_RLTV)/../spdk
ifneq ($(shell [ -d $(SPDK_DIR) ] && echo _spdk_exists),)
    INCLUDES += -I$(realpath $(SPDK_DIR)/include)
    CFLAGS += -DSUPPORT_SPDK=1
    # Use SPDK's build system for linking only, not compilation flags
    SPDK_ROOT_DIR := $(realpath $(SPDK_DIR))
    export SPDK_ROOT_DIR
    # Save current CFLAGS before including SPDK makefiles
    SAVED_CFLAGS := $(CFLAGS)
    include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
    include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk
    # Restore our CFLAGS and only use SPDK for linking
    CFLAGS := $(SAVED_CFLAGS)
    # Use whole-archive linking for SPDK libraries to resolve circular dependencies
    # Include bdev modules needed for malloc bdev functionality
    LFLAGS_SPDK_SRVR = -Wl,--whole-archive \
        $(addprefix $(SPDK_ROOT_DIR)/build/lib/libspdk_,$(addsuffix .a, \
            event event_bdev event_accel event_keyring event_vmd event_sock event_iobuf \
            bdev bdev_malloc bdev_null bdev_error bdev_gpt bdev_split bdev_delay \
            init rpc jsonrpc json util log trace notify accel dma thread scheduler_dynamic \
            env_dpdk sock keyring vmd)) \
        -Wl,--no-whole-archive $(ENV_LINKER_ARGS) $(SYS_LIBS)
endif

# ********** Define compilation/Linker flags *********
CFLAGS += -DCOMPILATION_DATE=${COMPILATION_DATE} -DCOMMIT_ID=0x$(COMMIT_ID)UL -DVER_TAGID=$(VER_TAGID) -DBRANCH_NAME=$(BRANCH_NAME)
CFLAGS += -Wall -Werror -Wextra -Wshadow -Werror=strict-aliasing -falign-functions=8 -std=c++2a
CFLAGS += -fPIC -fvisibility=hidden
ifeq ($(BUILD_RELEASE),1)
    USE_SANITIZERS?=0
    CFLAGS += -O2 -g -DNDEBUG
    TRACE_LEVEL?=5
else
    USE_SANITIZERS?=0
    CFLAGS += -g3 -ggdb3
    TRACE_LEVEL?=7
endif
ifeq ($(USE_SANITIZERS),1)
    CFLAGS += -fsanitize=leak -fsanitize=address -fsanitize=undefined
    LFLAGS_ALL += -fsanitize=leak -fsanitize=address -fsanitize=undefined
    USE_CLANG=0 # Clang does not support this yet
endif

ifeq ($(USE_THREAD_SANITIZER),1)
    CFLAGS += -fsanitize=thread
    LFLAGS_ALL += -fsanitize=thread
endif
CFLAGS += -DTRACE_LEVEL=$(TRACE_LEVEL)
LFLAGS__SO = -shared $(LFLAGS_EXT) -Wl,--no-undefined $(LFLAGS_ALL)
LFLAGS_EXE = -no-pie $(LFLAGS_ALL)
LFLAGS_EXE__STATIC = $(LFLAGS_EXT) # -static   Add this for container compilation to include libc++ and such in the exe
LFLAGS_EXE_DYNAMIC = -L$(INSTALL_DIR)/lib -l$(LIB_CLNT_NAME) -l$(LIB_SRVR_NAME)
ifeq ($(USE_CLANG)_$(shell clang -v 2>&1 | grep -c "clang version"), 1_1)
    CFLAGS += -Wno-vla-extension -Wno-null-pointer-subtraction
    #CFLAGS += -MJ $*.o.json
    CC=clang++
else
    CC=g++
endif

# ********** Actions *********
define print_compilation_info
	@printf "===========================================\n"
	@printf "\e[0;32mCompilation info\e[0;0m: Release=$(BUILD_RELEASE), COMMIT_ID=$(COMMIT_ID), $(CC), TRACE_LEVEL=$(TRACE_LEVEL) HT=$(HT)|\n"
	@if [ "$(USE_SANITIZERS)" = "1" ]; then \
		printf "\t* Notice \e[1;33mSanitizers enabled\e[0;0m, clang not supported\n"; \
	fi
    @printf "\t* Proj    | $(ROOT_DIR_ABS) [$(ROOT_DIR_RLTV)]\n"
    @printf "\t* CFLAGS  | $(CFLAGS)\n"
    @printf "\t* INCLUDS | $(INCLUDES)\n"
    @printf "\t* DFLAGS  | $(DFLAGS)\n"
    @printf "\t* L-FLexe | $(LFLAGS_EXE)\n"
    @printf "\t* L-FL.so | $(LFLAGS__SO)\n"
    @printf "\t* LDynExe | $(LFLAGS_EXE_DYNAMIC)\n"
    @printf "\t* LStaExe | $(LFLAGS_EXE__STATIC)\n"
    @printf "\t* LS_SPDK | $(LFLAGS_SPDK_SRVR)\n"
    @printf "===========================================\n"
    @printf "\t* INSTALL | Enabled=$(BUILD_ALSO_INSTALLS_DYN_LIBS), dir=$(INSTALL_DIR), AllowUring=$(ALLOW_USE_URING)\n"
    @printf "\t* ExeList | $(EXE_LIST_ALL)\n"
    @printf "===========================================\n"
endef

define print_synamic_dependencies
    @printf "Dynamic dependencies analysis:\n"
    @if [ "$(BUILD_ALSO_INSTALLS_DYN_LIBS)" = "1" ]; then \
        ldd $(OBJ_DIR)/$(LIB_CLNT_NAME).so $(OBJ_DIR)/$(LIB_SRVR_NAME).so; \
    else printf "\t*\e[1;33m.so libs not generated\e[0;0m\n"; \
    fi
    @ldd $(EXE_LIST_ALL)
    @printf "===========================================\n"
endef

help:
	$(call print_compilation_info);
	@printf "Usage |\e[0;32mmake all\e[0;0m| for building libs + executable unitest\n"
	@printf "Usage |\e[0;32mmake all BUILD_FOR_UNITEST=0 BUILD_RELEASE=1 BUILD_ALSO_INSTALLS_DYN_LIBS=1 VERBOSE=1\e[0;0m|\n"
	@printf "\n\n\n\n"

all: $(SOURCES_ALL) $(TARGET_BINARIES_RULE)
	$(if $(filter 1,$(VERBOSE)),$(call print_compilation_info))
	$(if $(filter 1,$(VERBOSE)),$(call print_synamic_dependencies))
	$(info +--->100% Done!)

define print_building_target
	@printf "+--->Building |\e[0;32m$@\e[0;0m|\n"
endef
define print_executed_rule
	@printf "   +--->: |\e[0;32m$@\e[0;0m|${1}\n"
endef
define link_executable
	$(ECHO_CMD) $(CC) -o $@ $^ $(LFLAGS_EXE) ${1} ${2} ${3} ${4};
	$(call print_executed_rule,"=Executable\\n");
endef

$(OBJ_DIR)/$(LIB_COMN_NAME).a: $(OBJECTS_BASE)
	$(call print_building_target);
	$(ECHO_CMD) ld -r $(OBJECTS_BASE) -o $@

$(OBJ_DIR)/$(LIB_CLNT_NAME).a: $(OBJ_DIR)/$(LIB_COMN_NAME).a $(OBJECTS_CLNT)
	$(call print_building_target);
	$(ECHO_CMD) ld -r $(OBJECTS_CLNT) -o $@

$(OBJ_DIR)/$(LIB_SRVR_NAME).a: $(OBJ_DIR)/$(LIB_COMN_NAME).a $(OBJECTS_SRVR)
	$(call print_building_target);
	$(ECHO_CMD) ld -r $(OBJECTS_SRVR) -o $@

$(OBJ_DIR)/$(LIB_CLNT_NAME).so: $(OBJECTS_BASE) $(OBJECTS_CLNT)
	$(call print_building_target);
	$(ECHO_CMD) $(CC) -o $@ $^ $(LFLAGS__SO)

$(OBJ_DIR)/$(LIB_SRVR_NAME).so: $(OBJECTS_BASE) $(OBJECTS_SRVR)
	$(call print_building_target);
	$(ECHO_CMD) $(CC) -o $@ $^ $(LFLAGS__SO)

$(UNITEST_CLNT_EXE): $(OBJECTS_TEST_CLNT) $(OBJ_DIR)/$(LIB_CLNT_NAME).a $(OBJ_DIR)/$(LIB_COMN_NAME).a $(OBJ_DIR)/$(LIB_SRVR_NAME).a
	$(call print_building_target);
	$(call link_executable, $(LFLAGS_EXE__STATIC))
	@printf "+-->Run Exe: \e[1;45m./$@ -h\e[0;0m\n"

$(UNITEST_CLNT_EXE)_dyn: $(OBJECTS_TEST_CLNT)
	$(call print_building_target);
	$(call link_executable, $(LFLAGS_EXE_DYNAMIC))
	@printf "+-->Run Exe: \e[1;45mLD_LIBRARY_PATH=$(INSTALL_DIR)/lib ./$@ -h\e[0;0m\n"

$(UNITEST_SPDK2_EXE): $(OBJECTS_SPDK_BOTH_APP) $(OBJ_DIR)/$(LIB_COMN_NAME).a $(OBJ_DIR)/$(LIB_SRVR_NAME).a $(OBJ_DIR)/$(LIB_CLNT_NAME).a
	$(call print_building_target);
	$(call link_executable, $(LFLAGS_EXE__STATIC), $(LFLAGS_SPDK_SRVR))
	@printf "+-->Run Exe: \e[1;45msudo ./$@\e[0;0m\n"

$(UNITEST_SPDKS_EXE): $(OBJECTS_SPDK_SRVR_APP)
	$(call print_building_target);
	$(call link_executable, $(LFLAGS_EXE_DYNAMIC), $(LFLAGS_SPDK_SRVR))
	@printf "+-->Run Exe: \e[1;45msudo LD_LIBRARY_PATH=$(INSTALL_DIR)/lib ./$@\e[0;0m\n"

$(OBJ_DIR)/%.o: %.cpp Makefile
	@mkdir -p $(dir $@)
	$(ECHO_CMD) $(CC) $(CFLAGS) $(INCLUDES) $(DFLAGS) -o $@ -c $<
	$(call print_executed_rule)

install: $(OBJ_DIR)/$(LIB_CLNT_NAME).so $(OBJ_DIR)/$(LIB_SRVR_NAME).so gusli_client_api.hpp gusli_server_api.hpp
	@printf "+-->Install to |\e[1;45m$(INSTALL_DIR)\e[0;0m|\n"
	@mkdir -p $(INSTALL_DIR)/lib $(INSTALL_DIR)/include;
#	@install -m 644 $(OBJ_DIR)/$(LIB_SRVR_NAME).a  $(INSTALL_DIR)/lib
	@install -m 755 $(OBJ_DIR)/$(LIB_CLNT_NAME).so $(INSTALL_DIR)/lib/lib$(LIB_CLNT_NAME).so
	@install -m 755 $(OBJ_DIR)/$(LIB_SRVR_NAME).so $(INSTALL_DIR)/lib/lib$(LIB_SRVR_NAME).so
	@install -m 644 gusli_client_api.hpp $(INSTALL_DIR)/include
	@install -m 644 gusli_server_api.hpp $(INSTALL_DIR)/include
#	@tree $(INSTALL_DIR)
#	ldconfig

uninstall:
	@printf "+-->Uninstall from |\e[1;45m$(INSTALL_DIR)\e[0;0m|\n"
	@rm -f $(INSTALL_DIR)/lib/lib$(PROJECT_PREFIX)_*
	@rm -f $(INSTALL_DIR)/lib/$(PROJECT_PREFIX)_*
	@rm -f $(INSTALL_DIR)/include/$(PROJECT_PREFIX)_*
#	ldconfig
# ********** Clean *********
define clean_compilation_intermediate
	@printf "+-->Cleaning recursive dir |\e[1;45m${1}/*/.[oda]\e[0;0m|\n"
	@find ./${1} -type f -name '*.[oda]' -delete
endef
define clean_compilation_output
	@printf "+-->Cleaning |\e[1;45mExe / Lib.so / CrashDumps\e[0;0m|\n"
	@find ./${1} -type f -name '*.so' -delete
	@rm -f $(UNITEST_EXE_PREFIX)$(PROJECT_PREFIX)* $(OBJ_DIR)/*.so core.*
endef

clean: uninstall
	@printf "+-->Cleaning |\e[1;45mall\e[0;0m|\n"
	$(call clean_compilation_intermediate,$(OBJ_DIR_ROOT))
	$(call clean_compilation_output)

.PHONY: depend clean uninstall
-include $(DEPEND_FILES_ALL)
