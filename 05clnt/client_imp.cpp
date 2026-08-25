/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdlib.h>
#include <errno.h>
#include "00utils/utils.hpp"
#include "client_imp.hpp"
#include "io_executors.hpp"
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>				// Definition of BLKGETSIZE64

namespace gusli {

global_clnt_context_imp& global_clnt_context_imp::get(void) noexcept {
	//static std::unique_ptr<global_clnt_context_imp> gc_ctx =  std::make_unique<global_clnt_context_imp>(); return *gc_ctx;
	static global_clnt_context_imp gc_ctx;
	return gc_ctx;
}
global_clnt_context_imp::global_clnt_context_imp() : base_library(LIB_NAME) {
	pr_info1("clnt_ctx[%p] - construct %lu[b]\n", this, sizeof(*this));
	shm_io_bufs = shm_io_bufs_global_t::get(LIB_NAME);
}
global_clnt_context_imp::~global_clnt_context_imp() {
	pr_info1("clnt_ctx[%p] - destruct\n", this);
	shm_io_bufs_global_t::put(LIB_NAME);
}

/************************************ parsing ********************************/
int global_clnt_context_imp::parse_conf(void) {
	if (par.config_file == NULL)
		return -__LINE__;
	const bool is_file_path = (strncmp(par.config_file, "# ", 2) ? true : false);
	char buf[1024], *buf_end = NULL;
	int parse_rv = 0;
	if (is_file_path) {
		FILE* f = fopen(par.config_file, "rt");
		if (f) {		// Read the config to buffer
			const long rv = fread(buf, 1, sizeof(buf)-1, f);
			if (rv >= 0) {
				buf_end = &buf[rv];
				buf[rv] = 0;
				if (!feof(f))
					parse_rv = -__LINE__;	// Buffer is too small to read the entire config
			} else {
				parse_rv = -__LINE__;
			}
			fclose(f);
		} else {
			parse_rv = -__LINE__;
		}
	} else {
		const int rv = strlen(par.config_file);
		if (rv >= (int)sizeof(buf))	// Buffer is too small to read the entire config
			parse_rv = -__LINE__;
		else
			strcpy(buf, par.config_file);
		buf_end = buf + rv;
	}
	if (parse_rv == 0) {
		pr_info1("library config: %s\n", buf);
		parse_rv = bdevs.parse_conf(buf, buf_end);
	}
	return parse_rv;
}

int bdev_config_params::init_parse(int version, const char* const argv[], int argc) noexcept {
	BUG_ON(version != 1, "Unsupported version type. Corrupted config?");
	int n_args_read = 0;
	char c;
	if (argc < 6) return -__LINE__;
	n_args_read += sscanf(argv[0], "%16s", id.set_invalid()->uuid);		// id is overrun by 1 byte with 0x0. Dont care as next field will be initialized later
	n_args_read += sscanf(argv[1], "%c", &c); type = (bdev_config_params::bdev_type)c;
	n_args_read += sscanf(argv[2], "%c", &c); how = (bdev_config_params::connect_how)c;
	n_args_read += sscanf(argv[3], "%c", &c); is_direct_io = (bool)((c == 'D')||(c == 'd'));
	n_args_read += sscanf(argv[4], "%63s", conn.any);	// %63s: leave room for the NUL terminator in conn.any[64] (was %64s -> 65-byte write)
	n_args_read += sscanf(argv[5], "%15s", security_cookie);
	// pr_verb1("+Dev: uuid=%s|%c|%c|%u|con=%s\n", id.uuid, type, how, is_direct_io, conn.any);
	if (n_args_read != argc) return -__LINE__;
	return (is_valid()) ? 0 : -__LINE__;
}

/************************************ API ********************************/
int global_clnt_context_imp::init(const global_clnt_context::init_params& _par, const char* metadata_json_format) noexcept {
	if (is_initialized()) {
		pr_err1("already initialized: %u[devices], doing nothing\n", bdevs.n_devices());
		return EEXIST;	// Success
	}
	#define abort_exe_init_on_err() { pr_err1("Initialization Error in line %d\n", __LINE__); shutting_down = true; free((char*)par.client_name); return -__LINE__; }
	par = _par;
	tDbg::log_file_set(par.log);
	par.client_name = _par.client_name ? strdup(_par.client_name) : strdup("client1");	// dup client name string
	if (!io_csring::is_big_enough_for(par.max_num_simultaneous_requests)) {
		pr_err1("Cannot support %u[ios] concurrently. Support at most %u\n.", par.max_num_simultaneous_requests, io_csring::get_capacity());
		abort_exe_init_on_err()
	}
	if (start() != 0)
		abort_exe_init_on_err()
	const int rv = parse_conf();
	pr_note1("initialized: %u devices, log_fd=%u rv=%d\n", bdevs.n_devices(), fileno(par.log), rv);
	if (rv != 0)
		abort_exe_init_on_err();
	sprintf(lib_info_json, metadata_json_format, LIB_NAME, __stringify(VER_TAGID) , COMMIT_ID, opt_level, TRACE_LEVEL, __stringify(COMPILATION_DATE));
	return rv;
}

int global_clnt_context_imp::destroy(void) noexcept {
	if (!is_initialized()) {
		pr_err1("not initialized, nothing to destroy\n");
		return ENOENT;
	}
	if (bdevs.has_any_bdev_open()) {
		pr_err1("Destroy aborted, client is still used\n");
		return -1;
	}
	free((char*)par.client_name);
	bdevs.clear();
	return finish(LIB_COLOR, 0);
}

enum connect_rv global_clnt_context_imp::bdev_connect(const backend_bdev_id& id) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	do_with_lock(bdev->control_path_lock);
	return bdev->connect(par.client_name, par.max_num_simultaneous_requests);
}

enum connect_rv server_bdev::connect(const char* client_name, const unsigned num_max_inflight_io) {
	static constexpr const mode_t blk_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;  // rw-r--r--
	server_bdev *bdev = this;
	const int o_flag = (O_RDWR | O_CREAT | O_LARGEFILE) | (bdev->conf.is_direct_io ? O_DIRECT : 0);
	pr_info1("Open " PRINT_BDEV_ID_FMT ", flag=0x%x, how=%c\n", PRINT_BDEV_ID_ARGS(*bdev), o_flag, bdev->conf.how);
	if (bdev->is_alive())
		return C_REMAINS_OPEN;
	bdev_info *info = &bdev->b.info;
	info->clear();
	info->num_max_inflight_io = num_max_inflight_io;
	if (bdev->conf.is_dummy()) {
		info->bdev_descriptor = 100000001;
		info->block_size = 4096;
		info->num_max_inflight_io = std::min(4U, num_max_inflight_io);
		strcpy(info->name, (bdev->conf.type == bdev_config_params::bdev_type::DUMMY_DEV_FAIL) ? "FAIL_DEV" : "STUCK_DEV");
		info->num_total_blocks = (1 << 10);		// 4[MB] dummy
		MGMT::msg_content msg;
		const int rv_dp_init = bdev->b.create_dp(bdev->conf.id, msg);
		return (rv_dp_init >= 0) ? C_OK : C_NO_RESPONSE;
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DEV_FS_FILE) {
		int access_how = F_OK | R_OK;
		if (bdev->conf.how == bdev_config_params::connect_how::EXCLUSIVE_RW) {
			access_how |= (W_OK /*| X_OK*/);	// Dont test for exclusive for now as it may not work without sudo
		} else if (bdev->conf.how == bdev_config_params::connect_how::SHARED_RW) {
			access_how |= (W_OK);
		}
		const int access_rv = access(bdev->conf.conn.any, access_how);
		if (access_rv == 0) {
			pr_info1("\tFile %s already exists, will not remove it upon close\n", bdev->conf.conn.any);
			info->flags_leak_fs_file_on_close = true;
		} else {
			pr_info1("\tFile %s does not exist, " PRINT_EXTERN_ERR_FMT "\n", bdev->conf.conn.any, PRINT_EXTERN_ERR_ARGS);
		}
		info->bdev_descriptor = open(bdev->conf.conn.any, o_flag, blk_mode);
		if (info->bdev_descriptor > 0) {
			info->block_size = 1;
			strcpy(info->name, "LocalFile");
			info->num_total_blocks = (1 << 30);	// 1[GB] file
			MGMT::msg_content msg;
			const int rv_dp_init = bdev->b.create_dp(bdev->conf.id, msg);
			return (rv_dp_init >= 0) ? C_OK : C_NO_RESPONSE;
		} else {
			pr_err1(PRINT_BDEV_ID_FMT " Cannot open! " PRINT_EXTERN_ERR_FMT "\n", PRINT_BDEV_ID_ARGS(*bdev), PRINT_EXTERN_ERR_ARGS);
			return C_NO_RESPONSE;
		}
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DEV_BLK_KERNEL) {
		info->bdev_descriptor = open(bdev->conf.conn.any, o_flag, blk_mode);
		if (info->bdev_descriptor > 0) {
			struct stat sb;
			const int rv0 = fstat(info->bdev_descriptor, &sb);
			if (rv0 != 0) {
				close(info->bdev_descriptor);
				return C_NO_DEVICE;
			}
			info->block_size = sb.st_blksize;
			snprintf(info->name, sizeof(info->name), "Kernel_bdev(%u,0x%x)", (int)sb.st_dev, (int)sb.st_rdev);
			if (sb.st_size) {
				info->num_total_blocks = sb.st_size;
			} else {
				const int rv1 = ioctl(info->bdev_descriptor, BLKGETSIZE64, &sb.st_size);
				if ((rv1 >=0) && (sb.st_size != 0)) {
					info->num_total_blocks = sb.st_size;
				} else {
					pr_err1(PRINT_BDEV_ID_FMT " Cannot determine size. Setting default!\n", PRINT_BDEV_ID_ARGS(*bdev));
					info->num_total_blocks = (1 << 30); // Default[GB]
				}
			}
			MGMT::msg_content msg;
			const int rv_dp_init = bdev->b.create_dp(bdev->conf.id, msg);
			return (rv_dp_init >= 0) ? C_OK : C_NO_RESPONSE;
		} else {
			pr_err1(PRINT_BDEV_ID_FMT " Cannot open! " PRINT_EXTERN_ERR_FMT "\n", PRINT_BDEV_ID_ARGS(*bdev), PRINT_EXTERN_ERR_ARGS);
			return C_NO_DEVICE;
		}
	} else if (bdev->conf.is_bdev_remote()) {
		bdev->b.hand_shake(bdev->conf, client_name);
		if (info->is_valid())
			return C_OK;
		return C_NO_RESPONSE;
	}
	return C_NO_DEVICE;
}

enum connect_rv global_clnt_context_imp::bdev_bufs_register(const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	if (bufs.empty())
		return C_WRONG_ARGUMENTS;			// Empty vector is invalid
	do_with_lock(bdev->control_path_lock);
	if (!bdev->is_alive())
		return C_NO_RESPONSE;
	return bdev->b.map_buf_do_vec(bufs);
}

enum connect_rv bdev_backend_api::bdev_ctl_log_msg1(const std::string &s) const noexcept {
	if (!sock.is_alive())
		return C_NO_RESPONSE;
	MGMT::msg_content msg;
	const size_t size = msg.build_log(s);
	const int send_rv = send_to(msg, size);
	return (send_rv < 0) ? C_NO_RESPONSE : C_OK;
}

enum connect_rv global_clnt_context_imp::bdev_ctl_log_msg2(const backend_bdev_id& id, const std::string &s) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	do_with_lock(bdev->control_path_lock);
	if (!bdev->is_alive())
		return C_NO_RESPONSE;
	if (!bdev->conf.is_bdev_remote()) {
		pr_info1(PRINT_BDEV_ID_FMT " Just local print %s\n", PRINT_BDEV_ID_ARGS(*bdev), s.c_str());
		return C_OK;
	}
	const enum connect_rv rv = bdev->b.bdev_ctl_log_msg1(s);
	if (rv != C_OK)
		pr_err1(PRINT_BDEV_ID_FMT " Error logging to server %s, rv=%d\n", PRINT_BDEV_ID_ARGS(*bdev), s.c_str(), rv);
	return rv;
}

enum connect_rv bdev_backend_api::bdev_ctl_reboot1(const std::string &s) noexcept {
	if (!sock.is_alive())
		return C_NO_RESPONSE;
	MGMT::msg_content msg;
	const size_t size = msg.build_reboot(s);
	const int send_rv = send_to(msg, size);
	return (send_rv < 0) ? C_NO_RESPONSE : C_OK;
}

void bdev_backend_api::bdev_ctl_reboot1_wait_for_srvr_disconnect(void) noexcept {
	wait_server_reply.wait(); // Wait for disconnection of server from client
}

class reconnect_to_server_task {
	server_bdev *bdev;
	const backend_bdev_id& id;
	pthread_t tid;
	static void* __exec(reconnect_to_server_task* me) {
		char old_name[32], new_name[32];	// Linux allows thread names of at most 16[b]
		auto *b = &me->bdev->b;
		const int srvr_prefix_len = (int)strncpy_no_trunc_warning(new_name, b->info.name, 12);
		strncpy_no_trunc_warning(&new_name[srvr_prefix_len], "cREC", 5);
		pthread_getname_np(pthread_self(), old_name, sizeof(old_name));
		pr_info1("\t\t\t%s[IO-Reconnect].p[%p].started, renaming %s->%s\n", b->info.name, me, old_name, new_name);
		const int rename_rv = pthread_setname_np(pthread_self(), new_name);
		if (rename_rv != 0)
			pr_err1("rename failed rv=%d " PRINT_EXTERN_ERR_FMT "\n", rename_rv, PRINT_EXTERN_ERR_ARGS);

		global_clnt_context_imp::get().bdev_stop_all_ios(me->id, true);
		pr_info1("\t\t\t%s[IO-Reconnect].p[%p].done\n", b->info.name, me);
		b->wait_server_io_enabled.done();
		delete me;
		return nullptr;
	}
 public:
	reconnect_to_server_task(server_bdev *_bdev) : bdev(_bdev), id(bdev->conf.id) {
		const int err = pthread_create(&tid, NULL, (void* (*)(void*))__exec, this);
		ASSERT_IN_PRODUCTION(err == 0);
	}
};

enum connect_rv global_clnt_context_imp::bdev_ctl_reboot2(const backend_bdev_id& id, const std::string &s) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	enum connect_rv rv;
	{
		do_with_lock(bdev->control_path_lock);
		if (!bdev->is_alive())
			return C_NO_RESPONSE;
		bdev->b.wait_server_io_enabled.reset();
		if (bdev->conf.is_bdev_remote()) {
			pr_info1(PRINT_BDEV_ID_FMT " ServerReboot.reason[%s].send\n", PRINT_BDEV_ID_ARGS(*bdev), s.c_str());
			rv = bdev->b.bdev_ctl_reboot1(s);	// Server will disconnect from client, listener will fail and try reconnect
		} else {
			pr_info1(PRINT_BDEV_ID_FMT " Reboot is a local drain. Reason %s\n", PRINT_BDEV_ID_ARGS(*bdev), s.c_str());
			bdev->b.do_on_listener_thread_terminate();	// Simulate as in remote path, as if listener thread finished
			rv = C_OK;
		}
	} // Unlock
	if (rv == C_OK) {
		bdev->b.bdev_ctl_reboot1_wait_for_srvr_disconnect();
		pr_info1(PRINT_BDEV_ID_FMT " ServerReboot.reason[%s].waited_for_shutdown\n", PRINT_BDEV_ID_ARGS(*bdev), s.c_str());
		bdev->b.wait_server_io_enabled.wait(); // Wait for reconnection of server to client
	} else {
		pr_err1(PRINT_BDEV_ID_FMT " Error rebooting server %s, rv=%d\n", PRINT_BDEV_ID_ARGS(*bdev), s.c_str(), rv);
	}
	return rv;
}

enum connect_rv global_clnt_context_imp::bdev_stop_all_ios(const backend_bdev_id& id, bool do_reconnect) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	do_with_lock(bdev->control_path_lock);
	const bool is_alive = bdev->is_alive();
	if (!is_alive && !do_reconnect) {
		pr_info1(PRINT_BDEV_ID_FMT " Force stop on stopped blockdevice, do nothing!\n", PRINT_BDEV_ID_ARGS(*bdev));
		return C_OK;
	}

	std::vector<io_buffer_t> bufs;
	if (is_alive) {
		const uint32_t num_ios = bdev->b.dp->get_num_in_air_ios();
		if (/*!bdev->b.dp->is_still_used() &&*/ (num_ios == 0) && do_reconnect) {
			pr_info1(PRINT_BDEV_ID_FMT " Refresh does nothing!\n", PRINT_BDEV_ID_ARGS(*bdev));
			return C_OK;				// Nothing to refresh
		}
		if (bdev->conf.is_bdev_remote()) {
			pr_info1(PRINT_BDEV_ID_FMT " Force disconnect from server to stop getting io completions\n", PRINT_BDEV_ID_ARGS(*bdev));
			bdev->b.should_try_reconnect.set(false);		// Because we are going to explicitly reconnect
			bdev->b.force_break_connection();
		}

		pr_info1(PRINT_BDEV_ID_FMT " draining %u io's in air\n", PRINT_BDEV_ID_ARGS(*bdev), num_ios);
		server_io_req *stuck_io = bdev->b.dp->in_air.get_next_in_air_io();
		while (stuck_io) {	// For local block devices this loop may do less iterations than num_ios (because io's keep completing). For remote this is impossible because we broke the socket
			stuck_io->internal_cancel();
			stuck_io = bdev->b.dp->in_air.get_next_in_air_io();
		}

		// Save the list of registered bufs to reregister them later
		bufs = bdev->b.dp->registered_bufs_get_list();
		bdev->b.dp->registered_bufs_force_clean();

		const connect_rv rv_disconnect = bdev->disconnect(false);
		ASSERT_IN_PRODUCTION(rv_disconnect == connect_rv::C_OK);
	}

	if (do_reconnect) {
		const connect_rv rv_reconnect = bdev->connect(par.client_name, par.max_num_simultaneous_requests);
		ASSERT_IN_PRODUCTION(rv_reconnect == connect_rv::C_OK);
		pr_info1(PRINT_BDEV_ID_FMT " reregistering %u mem bufs\n", PRINT_BDEV_ID_ARGS(*bdev), (uint32_t)bufs.size());
		const connect_rv rv_reregister = bdev->b.map_buf_do_vec(bufs);
		ASSERT_IN_PRODUCTION(rv_reregister == connect_rv::C_OK);
	}
	return C_OK;
}

enum connect_rv global_clnt_context_imp::bdev_bufs_unregist(const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	if (bufs.empty())
		return C_WRONG_ARGUMENTS;			// Empty vector is invalid
	do_with_lock(bdev->control_path_lock);
	if (!bdev->is_alive())
		return C_NO_RESPONSE;
	const uint32_t num_ios = bdev->b.dp->get_num_in_air_ios();	// Should be 0. Attempt to unregister bufs with in air IO may lead to memory corruption. Prevent this
	if (num_ios) {
		pr_err1("Error " PRINT_BDEV_ID_FMT " attempt to unregister mem with %u io's in air. This is a wrong flow that may lead to memory corruption!\n", PRINT_BDEV_ID_ARGS(*bdev), num_ios);
		return C_WRONG_ARGUMENTS;
	}
	return bdev->b.map_buf_un_vec(bufs);
}

enum connect_rv server_bdev::disconnect(const bool do_suicide) {
	server_bdev *bdev = this;
	const backend_bdev_id& id = bdev->conf.id;
	enum connect_rv rv = C_OK;
	datapath_t<bdev_stats_clnt> *dp = bdev->b.dp;
	if (!bdev->is_alive()) {
		rv = C_NO_RESPONSE;
	} else if (dp->is_still_used()) {
		pr_err1("Error: name=%s, still have %u[mapped-buffers], %u[ios]\n", bdev->b.info.name, dp->get_num_mem_reg_ranges(), dp->get_num_in_air_ios());
		return C_REMAINS_OPEN;
	} else if (bdev->conf.is_dummy()) {
		bdev->b.disconnect(id, do_suicide);
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DEV_FS_FILE) {
		close(bdev->get_fd());
		if (bdev->b.info.flags_leak_fs_file_on_close) {
			pr_info1("bdev %s, leaking file %s\n", bdev->b.info.name, bdev->conf.conn.any);
		} else {
			const int remove_rv = remove(bdev->conf.conn.any);
			rv = (remove_rv == 0) ? C_OK : C_NO_RESPONSE;
		}
		bdev->b.disconnect(id, do_suicide);
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DEV_BLK_KERNEL) {
		close(bdev->get_fd());
		bdev->b.disconnect(id, do_suicide);
	} else if (bdev->conf.is_bdev_remote()) {
		bdev->b.disconnect(id, do_suicide);
	} else {
		rv = C_NO_DEVICE;
	}
	pr_info1("Close " PRINT_BDEV_ID_FMT ", rv=%d\n", PRINT_BDEV_ID_ARGS(*bdev), rv);
	bdev->get_fd() = -1;
	return rv;
}

enum connect_rv global_clnt_context_imp::bdev_disconnect(const backend_bdev_id& id) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	do_with_lock(bdev->control_path_lock);
	return bdev->disconnect(false);
}

void global_clnt_context_imp::bdev_ctl_report_di(const backend_bdev_id& id, uint64_t offset_lba_bytes) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (bdev) {
		pr_err1("Error: User reported data corruption on " PRINT_BDEV_ID_FMT ", lba=0x%lx[B]\n", PRINT_BDEV_ID_ARGS(*bdev), offset_lba_bytes);
		do_with_lock(bdev->control_path_lock);
		bdev->disconnect(true);
	} else {
		pr_err1("Error: User reported data corruption on unknown " PRINT_BDEV_UUID_FMT ", lba=0x%lx[B]\n", id.uuid, offset_lba_bytes);
	}
}

uint32_t global_clnt_context_imp::bdev_ctl_get_n_in_air_ios(const backend_bdev_id& id) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (bdev) {
		do_with_lock(bdev->control_path_lock);
		if (bdev->is_alive()) {
			const uint32_t rv = bdev->b.dp->get_num_in_air_ios();
			pr_verb1(PRINT_BDEV_ID_FMT ".n_ios=%u\n", PRINT_BDEV_ID_ARGS(*bdev), rv);
			return rv;
		}
	}
	return 0;
}

enum connect_rv global_clnt_context_imp::bdev_get_info(const backend_bdev_id& id, bdev_info *ret_val) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev) return C_NO_DEVICE;
	if (!bdev->is_alive()) return C_NO_RESPONSE;
	do_with_lock(bdev->control_path_lock);
	*ret_val = bdev->b.info;
	return C_OK;
}

enum connect_rv global_clnt_context_imp::bdev_set_info(const backend_bdev_id& id, const bdev_info *set_val) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev) return C_NO_DEVICE;
	if (!bdev->is_alive()) return C_NO_RESPONSE;
	if (!set_val->is_valid()) return C_WRONG_ARGUMENTS;
	do_with_lock(bdev->control_path_lock);
	if (bdev->conf.type != bdev_config_params::bdev_type::DEV_FS_FILE) {
		pr_err1("Error: This type of operation is not supported yet for " PRINT_BDEV_ID_FMT "\n", PRINT_BDEV_ID_ARGS(*bdev));
		return C_WRONG_ARGUMENTS;
	}
	nvTODO("Verify that this is correct, and set other fields as well");
	bdev->b.info.num_total_blocks = set_val->num_total_blocks;
	bdev->b.info.flags_leak_fs_file_on_close = set_val->flags_leak_fs_file_on_close;
	pr_note1(PRINT_BDEV_ID_FMT " setting num_blocks to 0x%lx, leak?=%u\n", PRINT_BDEV_ID_ARGS(*bdev), set_val->num_total_blocks, set_val->flags_leak_fs_file_on_close);
	return C_OK;
}

/********************************* global_clnt_context ********************************************/
global_clnt_context::global_clnt_context(const init_params& par) {
	auto& instance = global_clnt_context_imp::get();
	const int init_rv = instance.init(par, this->metadata_json_format);
	if (init_rv < 0)
		throw clnt_init_exception(init_rv, "Failed to initialize");
	else if (init_rv > 0)
		throw clnt_init_exception(init_rv, "Already initialized");
}

global_clnt_context::~global_clnt_context() noexcept {
	(void)global_clnt_context_imp::get().destroy();
}

const char *global_clnt_context::get_metadata_json(void) const noexcept {
	return global_clnt_context_imp::get().get_metadata_json();
}

enum connect_rv global_clnt_context::bdev_connect(const backend_bdev_id& id) const noexcept {
	return global_clnt_context_imp::get().bdev_connect(id);
}
enum connect_rv global_clnt_context::bdev_bufs_register(const backend_bdev_id& id, const mem_list& bufs) const noexcept {
	return global_clnt_context_imp::get().bdev_bufs_register(id, bufs);
}

enum connect_rv global_clnt_context::bdev_force_close(const backend_bdev_id& id, bool do_reconnect) const noexcept {
	return global_clnt_context_imp::get().bdev_stop_all_ios(id, do_reconnect);
}

enum connect_rv global_clnt_context::bdev_ctl_log_msg(const backend_bdev_id& id, const std::string &s) const noexcept {
	return global_clnt_context_imp::get().bdev_ctl_log_msg2(id, s);
}

enum connect_rv global_clnt_context::bdev_ctl_reboot(const backend_bdev_id& id, const std::string &s) const noexcept {
	return global_clnt_context_imp::get().bdev_ctl_reboot2(id, s);
}

enum connect_rv global_clnt_context::bdev_bufs_unregist(const backend_bdev_id& id, const mem_list& bufs) const noexcept{
	return global_clnt_context_imp::get().bdev_bufs_unregist(id, bufs);
}

enum connect_rv global_clnt_context::bdev_disconnect(const backend_bdev_id& id) const noexcept {
	return global_clnt_context_imp::get().bdev_disconnect(id);
}

enum connect_rv global_clnt_context::open__bufs_register(const backend_bdev_id& id, const mem_list& bufs) const noexcept {
	global_clnt_context_imp &c = global_clnt_context_imp::get();
	server_bdev *bdev = c.bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	do_with_lock(bdev->control_path_lock);
	if (!bdev->is_alive()) {						// Try to auto open bdev
		const enum connect_rv rv = c.bdev_connect(id);
		if (rv != C_OK)
			return rv;
	}
	return c.bdev_bufs_register(id, bufs);
}
enum connect_rv global_clnt_context::close_bufs_unregist(const backend_bdev_id& id, const mem_list& bufs, bool stop_server) const noexcept {
	global_clnt_context_imp &c = global_clnt_context_imp::get();
	server_bdev *bdev = c.bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	do_with_lock(bdev->control_path_lock);
	if (!bufs.empty()) {
		const enum connect_rv rv = c.bdev_bufs_unregist(id, bufs);
		if (rv != C_OK)
			return rv;
	}
	if (!bdev->b.dp->is_still_used())
		bdev->disconnect(stop_server);		// User has nothing to do with close failure
	return C_OK;
}

enum connect_rv global_clnt_context::bdev_get_info(const backend_bdev_id& id, bdev_info &rv) const noexcept {
	return global_clnt_context_imp::get().bdev_get_info(id, &rv);
}

enum connect_rv global_clnt_context::bdev_override_info(const backend_bdev_id& id, const bdev_info &val) noexcept {
	return global_clnt_context_imp::get().bdev_set_info(id, &val);
}

int32_t global_clnt_context::bdev_get_descriptor(const backend_bdev_id& id) const noexcept {
	bdev_info rv;
	if (global_clnt_context_imp::get().bdev_get_info(id, &rv) == connect_rv::C_OK)
		return rv.bdev_descriptor;
	return -1;
}

void global_clnt_context::bdev_ctl_report_data_corruption(const backend_bdev_id& id, uint64_t offset_lba_bytes) const noexcept {
	global_clnt_context_imp::get().bdev_ctl_report_di(id, offset_lba_bytes);
}

uint32_t global_clnt_context::bdev_ctl_get_num_in_air_ios(const backend_bdev_id& id) const noexcept {
	return global_clnt_context_imp::get().bdev_ctl_get_n_in_air_ios(id);
}

/*****************************************************************************/
/* IO api race conditions after io was submitted. Worst case - it has an async callback and it is polled:
	* Multiple user different threads poll IO status via get_error().
		* Extra race in polling mode io, where this function actually creates progress and not treats io as const
		* IO executor is responsible for the poling critical section
	* Multiple user different threads call cancel_wait() due to timeout or any other reason.
		* IO executor serialized those requests
	* When IO completes and gives callback to user. This callback can also execute get_error() or cancel_wait()
		* It will definitely call get_error() at least once
		* Calling cancel make no sense from callback but user can do this (will get io already done as reply).
	* Incoming io completions from Server
		* With server backend - completion ring wakes up and calls client_receive_server_finish_io(), race with cancel and stop all io's
		* IO executor ris responsible for managing executor completions separately from user requests (like cancel / io poling)
	* Stop all ios (Server disconnect and reconnect)
		* Generate internal cancellation. Behaves like user requested cancel but on all ios.
	* Once cancel_wait() succeeds, IO can be freed so callback must not arrive on canceled ios

  Key points:
	* Once IO status becomes not in transfer (success / failure) user can immediately call done() and free it (possibly polling thread).
	* This implies that in the code we cannot refer to io after rv was set.
  User potential miss-behaving that will cause memory corruption: (GUSLI does not protect against it)
	* Call done() and freeing io from completion callback. This is a bad practice because user may
		* Poll the same io status from a different thread. done() will be called while another thread does polling
		* Another thread may run { cancel() / done() } / stop all io and have a race of which thread call done()
	* Polling IO from 1 thread in parallel to callback arriving on another thread.
		* Inherent race condition: setting io rv and issuing callback. GUSLI assumes that polling is potentially done so once
			rv of io is set, immediately done() can be called and io can be free() from the polling thread.
	* Calling done() and freeing the io too early with async io. A callback of completion may arrive before submit_io() finished
		It is not advisable to free the io before user submission finished as even harmless prints ("submitted successfully")
		May cause use after free
*/
void server_io_req::client_receive_server_finish_io(int64_t rv) {
	_exec->extern_notify_completion(rv);	// Here is a race condition with already canceled io, todo, fix me
}

void io_request_base::submit_io(void) noexcept {
	BUG_ON((out.rv == io_error_codes::E_IN_TRANSFER) ||
			(_exec != NULL), "memory corruption: attempt to retry io[%p] while it is still running. Before retrying wait for termination or cancel and call done()!", this);
	server_io_req *sio = (server_io_req*)this;
	sio->start_execution();
	const server_bdev *bdev = NULL;
	if (params.get_bdev_descriptor() > 0)
		bdev = global_clnt_context_imp::get().bdevs.find_by_io(params.get_bdev_descriptor());
	if (unlikely(!bdev)) {
		pr_err1("Error: Invalid bdev descriptor of io=%d. Open bdev to obtain a valid descriptor\n", params.get_bdev_descriptor());
		io_autofail_executor(*this, io_error_codes::E_INVAL_PARAMS);
		return;
	}
	sio->is_remote_set(bdev->conf.is_bdev_remote());
	if (bdev->conf.is_bdev_local()) {
		if (!params.is_safe_io()) {
			if (!bdev->b.dp->verify_io_param_valid(*sio)) {
				io_autofail_executor(*this, io_error_codes::E_INVAL_PARAMS);
				return;
			}
		}
		#if defined(HAS_URING_LIB)
			if (!params.has_callback() && params.may_use_uring())	// Uring does not support async callback mode
				_exec = new uring_request_executor(bdev->b.dp->in_air, *this);
		#endif
		if (!_exec) {
			if (!params.is_blocking_io()) {					// Async IO, with / without completion
				_exec = new aio_request_executor(bdev->b.dp->in_air, *this);
			} else {										// Blocking IO
				_exec = new sync_request_executor(bdev->b.dp->in_air, *this);
			}
		}
		_exec->run();									// Will auto delete exec upon IO finish;
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DUMMY_DEV_FAIL) {
		io_autofail_executor(*this, io_error_codes::E_PERM_FAIL_NO_RETRY);	// Here, injection of all possible errors
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DUMMY_DEV_STUCK) {
		_exec = new never_reply_executor(bdev->b.dp->in_air, *this);
		_exec->run();
	} else if (bdev->conf.is_bdev_remote()) {
		const bool should_block = params.is_blocking_io();
		if (unlikely(should_block)) {
			_exec = new wrap_remote_io_exec_blocking(bdev->b.dp->in_air, *this);
		} else {
			_exec = new wrap_remote_io_exec_async(bdev->b.dp->in_air, *this);
		}
		if (_exec->run() < 0)
			return;
		bool need_wakeup_srvr;
		const int send_rv = bdev->b.dp->clnt_send_io(*sio, &need_wakeup_srvr);
		if (send_rv >= 0) {
			nvTODO("Here completion from server could already arrived and access to *this below is potentially use after free");
			pr_verb1(PRINT_IO_REQ_FMT PRINT_IO_SQE_ELEM_FMT PRINT_CLNT_IO_PTR_FMT ", doorbell=%d\n", PRINT_IO_REQ_ARGS(params), send_rv, this, need_wakeup_srvr);
			if (need_wakeup_srvr) {
				const int wakeup_rv = bdev->b.dp_wakeup_server();
				if (wakeup_rv < 0)
					pr_err1(PRINT_IO_REQ_FMT PRINT_IO_SQE_ELEM_FMT PRINT_CLNT_IO_PTR_FMT ", error waking up server, may stuck...\n", PRINT_IO_REQ_ARGS(params), send_rv, this);
			}
			// Callback will come in future
		} else { // Submission failed, Give callback
			((server_io_req *)this)->client_receive_server_finish_io(send_rv);
		}
		if (should_block)
			_exec->is_still_running();		// Blocking wait, for success or failure
	} else {
		io_autofail_executor(*this, io_error_codes::E_INVAL_PARAMS);
	}
}

enum io_error_codes io_request_base::get_error(void) noexcept {
	if (out.rv == io_error_codes::E_IN_TRANSFER) {
		DEBUG_ASSERT(!params.is_blocking_io());		// Impossible for blocking io as out.rv would be already set
		if (params.is_polling_mode()) {
			BUG_ON(!_exec, "IO has not finished yet, It must have a valid executor, rv=%ld", out.rv);
			if (_exec->is_still_running() == io_error_codes::E_IN_TRANSFER)
				return io_error_codes::E_IN_TRANSFER;
			// Here executor terminated but maybe did not update the io rv yet, so check it again.
			if (out.rv == io_error_codes::E_IN_TRANSFER)
				return io_error_codes::E_IN_TRANSFER;
		} else {
			return io_error_codes::E_IN_TRANSFER;	// Cannot touch async executor
		}
	}
	return (out.rv > 0) ? io_error_codes::E_OK : (enum io_error_codes)out.rv;
}

enum io_request::cancel_rv io_request_base::cancel_wait(void /*bool blocking_wait*/) noexcept {
	if (out.rv != io_error_codes::E_IN_TRANSFER)
		return io_request::cancel_rv::G_ALLREADY_DONE;
	DEBUG_ASSERT(!params.is_blocking_io());		// Impossible for blocking io as out.rv would be already set
	const enum cancel_rv crv = _exec->cancel();	// Instruct executor to fail as fast as possible
	while (_exec->is_still_running() == io_error_codes::E_IN_TRANSFER) {
		std::this_thread::sleep_for(std::chrono::microseconds(10));	// Block until executor finishes
		nvTODO("Todo: Once every 5[sec] print here something to log");
	}
	(void)_exec->cancel();	// For async executor - wait for it to give callback to user. Already done
	if (crv == cancel_rv::G_CANCELED) {
		out.rv = (int64_t)io_error_codes::E_CANCELED_BY_CALLER;
	} // Else: Already done during call to executor cancel
	return crv;
}

void server_io_req::internal_cancel(void) noexcept {  // Called when external completions (from srvr) cannot arrive anymore
	pr_verb1(PRINT_EXECUTOR "io[%c].draining.uid[%u]\n", _exec, this, params.op(), unique_id_get());
	if (out.rv != io_error_codes::E_IN_TRANSFER)
		return;
	(void)_exec->cancel();	// Executor will not return callback. IO might be already done so no-op in this case
	if (_exec->is_still_running() == io_error_codes::E_IN_TRANSFER) {	// Completion will not come so simulate it
		_exec->mark_not_in_air();
		_exec->extern_notify_completion((int64_t)io_error_codes::E_CANCELED_BY_CALLER);
		out.rv = (int64_t)io_error_codes::E_CANCELED_BY_CALLER;
		// Here user can call io done and free it!
	}
}

void io_request_base::done(void) noexcept {
	ASSERT_IN_PRODUCTION(out.rv != io_error_codes::E_IN_TRANSFER);		// IO has finished
	if (_exec) {
		_exec->detach_io();	// Disconnect executor from io (and free executor)
		_exec = nullptr;
	}
}

io_request::~io_request() {				// Needed because user may not call get error nor cancel and executor will be stuck. So force call get error
	if (unlikely(out.rv == io_error_codes::E_IN_TRANSFER)) {
		pr_err1(PRINT_IO_REQ_FMT PRINT_CLNT_IO_PTR_FMT ", User wrong flow, Destroying io while it is still in air (running)\n", PRINT_IO_REQ_ARGS(params), this);
		if (params.is_polling_mode()) {
			const cancel_rv crv = cancel_wait();
			pr_err1(PRINT_EXECUTOR PRINT_IO_REQ_FMT ", cancel_rv=%d\n", _exec, this,  PRINT_IO_REQ_ARGS(params), crv);
		}
		BUG_ON(out.rv == io_error_codes::E_IN_TRANSFER, "Destroying io while it is still in air (running)!");
	}
	if (unlikely(_exec)) {
		pr_err1(PRINT_EXECUTOR PRINT_IO_REQ_FMT ", User wrong flow, Started IO and never checked that it finished (rv=%ld). Force cleanup\n", _exec, this, PRINT_IO_REQ_ARGS(params), out.rv);
		done();
	}
}

/************************* communicate with server *****************************************/
int bdev_backend_api::send_to(MGMT::msg_content &msg, size_t n_bytes) const {
	if (msg.is(MGMT::msg::dp_submit))
		pr_verb1(" >> %s: %s\n", srv_addr, msg.raw());
	else
		pr_info1(" >> %s: %s\n", srv_addr, msg.raw());
	//ssize_t send_rv = sock.send_msg(msg.raw(), n_bytes, ca);
	return (ios_ok == __send_1_full_message(sock, msg, false, n_bytes, ca, LIB_NAME)) ? 0 : -1;
}

int bdev_backend_api::create_dp(const backend_bdev_id &id, MGMT::msg_content &msg) {
	if (!info.is_valid())
		return -__LINE__;
	const size_t size = msg.build_reg_buf();
	auto *pr = &msg.pay.c_register_buf;
	// Initialize datapath of producer
	const uint64_t n_bytes = io_csring::n_needed_bytes(info.block_size);
	pr->build_scheduler(id, n_bytes / (uint64_t)info.block_size);
	wait_server_reply.reset();
	if (has_remote()) {
		dp = new datapath_t<bdev_stats_clnt>(pr->name, global_clnt_context_imp::get().shm_io_bufs, info);
		*(uint64_t*)dp->shm_ring.get_buf() = MGMT::shm_cookie;		// Insert cookie for the server to verify
		const io_buffer_t buf = io_buffer_t::construct(dp->shm_ring.get_buf(), dp->shm_ring.get_n_bytes());
		pr_verb1(PRINT_REG_BUF_FMT ", to=%s\n", PRINT_REG_BUF_ARGS(pr, buf), srv_addr);
		if (send_to(msg, size) < 0) {
			delete dp; dp = nullptr;
			return -__LINE__;
		}
		check_incoming();
		wait_server_reply.wait();
	} else {	// Same datapath but without server side
		dp = new datapath_t<bdev_stats_clnt>(nullptr, global_clnt_context_imp::get().shm_io_bufs, info);
	}
	return 0;
}

int bdev_backend_api::hand_shake(const bdev_config_params &conf, const char *clnt_name) {
	srv_addr = conf.conn.any;
	const sock_t::type s_type = MGMT::get_com_type(srv_addr);
	MGMT::msg_content msg;
	int conn_rv;
	const bool blocking_connect = true; // (s_type != sock_t::type::S_UDP);
	pr_info1("Connecting to |%s|, blocking=%u\n", srv_addr, blocking_connect);
	if (     s_type == sock_t::type::S_UDP) conn_rv = sock.clnt_connect_to_srvr_udp(MGMT::COMM_PORT, &srv_addr[1], ca, blocking_connect);
	else if (s_type == sock_t::type::S_TCP) conn_rv = sock.clnt_connect_to_srvr_tcp(MGMT::COMM_PORT, &srv_addr[1], ca, blocking_connect);
	else if (s_type == sock_t::type::S_UDS) conn_rv = sock.clnt_connect_to_srvr_uds(                  srv_addr   , ca, blocking_connect);
	else {
		pr_err1("unsupported server address |%s|\n", srv_addr);
		info.bdev_descriptor = -__LINE__;
		return info.bdev_descriptor;
	}
	if (conn_rv < 0) {
		pr_err1("Cannot connenct to |%s|, rv=%d. is server up?\n", srv_addr, conn_rv);
		info.bdev_descriptor = -1;
		goto _out;
	}
	{
		const size_t size = msg.build_hello();
		auto *p = &msg.pay.c_hello;
		p->fill(conf.id, clnt_name, conf.security_cookie);
		if (send_to(msg, size) >= 0)
			check_incoming();
		else
			goto _out;
	}
	if (create_dp(conf.id, msg) < 0)
		goto _out;
	if (MGMT::set_large_io_buffers)
		sock.set_io_buffer_size(1<<19, 1<<19);
	info.bdev_descriptor = sock.fd();
	{
		const int err = pthread_create(&io_listener_tid, NULL, (void* (*)(void*))io_completions_listener, this);
		ASSERT_IN_PRODUCTION(err == 0);
	}
_out:
	if (!info.is_valid())
		disconnect(conf.id);				// Cleanup open with close
	return info.bdev_descriptor;
}

int bdev_backend_api::map_buf(const io_buffer_t io) {
	if (!io.is_valid_for(info.block_size))
		return -__LINE__;
	MGMT::msg_content msg;
	const size_t size = msg.build_reg_buf();
	{
		do_with_lock(dp->shm_io_bufs->with_lock());
		const base_shm_element *g_map = dp->shm_io_bufs->insert_on_client(io);
		if (!g_map)
			return -__LINE__;			// Buffer rejected by global hash
		if (!dp->reg_bufs_set.add(g_map->buf_idx)) {
			pr_err1("%s: Wrong register buf request to " PRINT_IO_BUF_FMT ", it is already registered to this bdevs as " PRINT_REG_IDX_FMT "\n", info.name, PRINT_IO_BUF_ARGS(io), g_map->buf_idx);
			dp->shm_io_bufs->dec_ref(g_map, false);	// Abort, dec ref. Verify cannot free the global because its ref must be >= 2
			return -__LINE__;			// Already mapped to this block device
		}
		auto *pr = &msg.pay.c_register_buf;
		// Note: Dont touch *(uint64_t*)g_map->mem.get_buf() - It might be used now with io to a different bdev, possible on different server
		pr->build_io_buffer(*g_map, info.block_size);
		pr_info1(PRINT_REG_BUF_FMT ", name=%s, ref_cnt[%u]\n", PRINT_REG_BUF_ARGS(pr, io), pr->name, g_map->ref_count);
	}	// Release lock here
	if (has_remote()) {
		if (send_to(msg, size) < 0)
			return -__LINE__;
		wait_server_reply.wait();
		// Note: Here server may disconnect and reconnect from client but buffers list is already set
	}
	return 0;
}

enum connect_rv bdev_backend_api::map_buf_do_vec(const std::vector<io_buffer_t>& bufs) {
	for (size_t i = 0; i < bufs.size(); i++) {
		const int map_rv = map_buf(bufs[i]);
		if (map_rv != 0)
			return C_WRONG_ARGUMENTS;
	}
	return C_OK;
}

int bdev_backend_api::map_buf_un(const io_buffer_t io) {
	MGMT::msg_content msg;
	const size_t size = msg.build_unr_buf();
	{
		do_with_lock(dp->shm_io_bufs->with_lock());
		const base_shm_element *g_map = dp->shm_io_bufs->find1(io);
		if (!g_map) {
			pr_err1("Wrong unregister buf request to " PRINT_IO_BUF_FMT ", it does not exist\n", PRINT_IO_BUF_ARGS(io));
			return -__LINE__;
		}
		if (!dp->reg_bufs_set.has(g_map->buf_idx)) {
			pr_err1("Wrong unregister buf request to " PRINT_IO_BUF_FMT ", it is registered to other bdevs, not to this one\n", PRINT_IO_BUF_ARGS(io));
			return -__LINE__;
		}
		auto *pr = &msg.pay.c_unreg_buf;
		pr->build_io_buffer(*g_map, info.block_size);
		pr_info1(PRINT_UNR_BUF_FMT ", name=%s, srvr_ptr=%p, ref_cnt[%u]--\n", PRINT_REG_BUF_ARGS(pr, io), pr->name, g_map->other_party_ptr, g_map->ref_count);

		dp->reg_bufs_set.del(g_map->buf_idx);
		dp->shm_io_bufs->dec_ref(g_map);
	}	// Release lock here
	if (has_remote()) {
		if (send_to(msg, size) < 0)
			return -__LINE__;
		wait_server_reply.wait();
		// Note: Here server replied or disconnected. Upon reconnect buffers list should be updated
	}
	return 0;
}

enum connect_rv bdev_backend_api::map_buf_un_vec(const std::vector<io_buffer_t>& bufs) {
	for (int i = (int)bufs.size()-1; i >= 0; i--) {			// Assuming same vector as register - do reverse order to reduce vector moves
		const int map_rv = map_buf_un(bufs[i]);
		if (map_rv != 0)
			return C_WRONG_ARGUMENTS;
	}
	return C_OK;
}

int bdev_backend_api::disconnect(const backend_bdev_id& id, const bool do_kill_server) {
	if (io_listener_tid) {
		int send_rv = 0;
		if (sock.is_alive()) {
			should_try_reconnect.set(false);		// Because we are explicitly disconnecting, no reconnection needed
			MGMT::msg_content msg;
			size_t size;
			if (do_kill_server) {
				size = msg.build_die();
			} else {
				size = msg.build_close();
				msg.pay.c_close.volume = id;
			}
			send_rv = send_to(msg, size);
			if (send_rv < 0)
				pr_err1(PRINT_BDEV_UUID_FMT " Unable to disconnect from server %s, rv=%d\n",  id.uuid, srv_addr, send_rv);
		}
		pr_info1("going to join listener_thread tid=0x%lx, sock_alive=%d\n", (long)io_listener_tid, sock.is_alive());
		const int err = pthread_join(io_listener_tid, NULL);
		ASSERT_IN_PRODUCTION(err == 0);
	}
	delete dp; dp = nullptr;
	clean_srvr();
	return 0;
}

void bdev_backend_api::check_incoming(void) {
	connect_addr addr = this->ca;
	MGMT::msg_content msg;
	const enum io_state io_st = __read_1_full_message(sock, msg, false, addr);
	if (io_st != ios_ok) {
		pr_info1("receive type=%c, io_state=%d, " PRINT_EXTERN_ERR_FMT "\n", sock.get_type(), io_st, PRINT_EXTERN_ERR_ARGS);
		if (io_state_broken(io_st))
			is_control_path_ok = false;
		BUG_ON(io_st == ios_block, "Client listener should have blocking wait for io completion, It has no other job to do");
		return;
	}
	char server_path[32];
	sock.print_address(server_path, addr);	// Todo: Multiple servers, find the correct one
	if (msg.is(MGMT::msg::dp_complete))
		pr_verb1("<< |%s|\n", msg.raw());
	else
		pr_info1("<< |%s| from %s\n", msg.raw(), server_path);
	if (true) {
		on_keep_alive_received(); // Every message is considered a keep alive
		if (msg.is(MGMT::msg::keepalive)) {
			return;
		} else if (msg.is(MGMT::msg::hello_ack)) {
			this->info = msg.pay.s_hello_ack.info;
			ASSERT_IN_PRODUCTION(io_csring::is_big_enough_for(info.num_max_inflight_io));
			ASSERT_IN_PRODUCTION(info.is_valid());
		} else if (msg.is(MGMT::msg::register_ack)) {
			const auto *pr = &msg.pay.s_register_ack;
			if (pr->is_io_buf) {
				do_with_lock(dp->shm_io_bufs->with_lock());
				BUG_ON(!dp->shm_io_bufs->find2(pr->buf_idx), "Server gave ack on unknown registered memory buf_idx=%u, srvr_ptr=0x%lx\n", pr->buf_idx, pr->server_pointer);
			}
			pr_info1("RegisterAck[%d%c] name=%s, srvr_ptr=0x%lx, rv=%d\n", pr->get_buf_idx(), pr->get_buf_type(), pr->name, pr->server_pointer, pr->rv);
			BUG_ON(pr->rv != 0, "rv=%d", pr->rv);
			wait_server_reply.done();	// Unlock caller which waits for buffer registration
		} else if (msg.is(MGMT::msg::unreg_ack)) {
			const auto *pr = &msg.pay.s_unreg_ack;
			pr_info1("UnRegistAck[%d%c] name=%s, srvr_ptr=0x%lx, rv=%d\n", pr->get_buf_idx(), pr->get_buf_type(), pr->name, pr->server_pointer, pr->rv);
			BUG_ON(pr->rv != 0, "rv=%d", pr->rv);
			wait_server_reply.done();	// Unlock caller which waits for buffer un-registration
		} else if (msg.is(MGMT::msg::server_kick)) {
			BUG_NOT_IMPLEMENTED();
		} else if (msg.is(MGMT::msg::close_ack)) {
			pr_info1("\t%s remote closed\n", this->info.name);
			is_control_path_ok = false;
		} else if (msg.is(MGMT::msg::die_ack)) {
			pr_info1("\t%s Server dies, reason:%s\n", this->info.name, msg.pay.s_die.extra_info);
			is_control_path_ok = false;
		} else if (msg.is(MGMT::msg::dp_complete)) {
			// Drain all awaiting completions
			bool need_wakeup_srvr = false, wakeup_on_msg;
			int idx = dp->clnt_receive_completion(&wakeup_on_msg);
			for (; idx >= 0; idx = dp->clnt_receive_completion(&wakeup_on_msg)) {
				need_wakeup_srvr |= wakeup_on_msg;
				pr_verb1(PRINT_IO_CQE_ELEM_FMT " processed completion. Need_wakeup=%d\n", idx, wakeup_on_msg);
			}
			if (need_wakeup_srvr) { // We dont send more than ring size ios so server never runs out of completion entries
				// Find bdev by message address and call bdev->b.dp_wakeup_server()); // because client is ready to accept new completions
				pr_err1("%s: completion list full (throttling not implemented). IO may stuck. Probably user app sent too much IO's in air (more than allowed)...\n", server_path);
			}
		} else {
			BUG_ON(true, "Unhandled message %s\n", msg.hdr.type);
		}
	}
	return;
}

int bdev_backend_api::dp_wakeup_server(void) const {
	MGMT::msg_content msg;
	const size_t size = msg.build_dp_subm();
	auto *p = &msg.pay.dp_submit;
	p->sender_added_new_work = true;
	p->sender_ready_for_work = false;
	dp->stats.n_doorbells_wakeup_srvr++;
	return send_to(msg, size);
}

void* bdev_backend_api::io_completions_listener(bdev_backend_api *bdev) {
	{	// Rename thread
		char old_name[32], new_name[32];	// Linux allows thread names of at most 16[b]
		const int srvr_prefix_len = (int)strncpy_no_trunc_warning(new_name, bdev->info.name, 12);
		strncpy_no_trunc_warning(&new_name[srvr_prefix_len], "cIOl", 5);
		pthread_getname_np(pthread_self(), old_name, sizeof(old_name));
		pr_info1("\t\t\t%s[Listener].started, renaming %s->%s\n", bdev->info.name, old_name, new_name);
		const int rename_rv = pthread_setname_np(pthread_self(), new_name);
		if (rename_rv != 0)
			pr_err1("rename failed rv=%d " PRINT_EXTERN_ERR_FMT "\n", rename_rv, PRINT_EXTERN_ERR_ARGS);
	}
	for (bdev->is_control_path_ok = true; bdev->is_control_path_ok; ) {
		bdev->check_incoming();
	}
	bdev->do_on_listener_thread_terminate();
	return NULL;
}

void bdev_backend_api::do_on_listener_thread_terminate(void) {
	server_bdev *s = container_of(this, server_bdev, b);
	wait_server_reply.done();	// Wakeup whoever waited for a server reply but will not get it because connection is broken / listener thread existed
	const uint32_t should_reconnect = should_try_reconnect.read();
	pr_info1("\t\t\t%s[Listener].addr[%s].end.reconnect[%d]\n", info.name, srv_addr, should_reconnect);
	if (should_reconnect)
		new reconnect_to_server_task(s);
}

}	// namespace
