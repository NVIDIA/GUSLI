#pragma once
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
/* Gusli client: Block IO submission api */
#include <stdio.h>			// FILE, printf
#include <string.h>			// memset, memcmp
#include <stdint.h>			// uint64_t, uint32_t
#include <vector>
#include <stdexcept>

#define SYMBOL_EXPORT __attribute__((__visibility__("default")))
#define SYMBOL_EXPORT_NO_DISCARD	[[nodiscard]] SYMBOL_EXPORT
namespace gusli {

/***************************** block device configuration ********************/
// Note: Library does not manage local block devices, it assumes they exist and library can connect to them. Like local disks
struct backend_bdev_id {					// Unique ID of volume / block device / disk
	char uuid[16];							// Deliberately no dependency on uuid.lib
	friend bool operator==(const backend_bdev_id& a, const backend_bdev_id& b) noexcept {
		return (memcmp(a.uuid, b.uuid, sizeof(a.uuid)) == 0);
	}
	backend_bdev_id *set_invalid(void) { memset(uuid, 0, sizeof(uuid)); return this; }
	void set_from(const char* str) { set_invalid(); const size_t nc = std::min(strlen(str) + 1, sizeof(uuid)); memcpy(uuid, str, nc); }
	void set_from(const uint64_t uid) { set_invalid(); snprintf(uuid, sizeof(uuid), "%lu", uid); }
	bool is_valid(void) const { return (uuid[0] != 0); }
} __attribute__((aligned(sizeof(long))));

struct bdev_info {							// After connection to bdev established, this info can be retrieved
	char name[32];							// Server self reported name, used for logging / debug only.
	int32_t  bdev_descriptor;				// Much like file descriptor, Used to access this bdev in datapath. 0 and negative are invalid
	uint32_t block_size;					// In [bytes]. Minimal unit for IO (size and alignment). Typically 4[KB]..16[MB], 1[B] for files
	uint64_t num_total_blocks;				// Number of blocks accessible for IO. Can be extended in runtime, never shrunk.
	uint16_t num_max_inflight_io;			// QOS. More than this amount will be throttled by the client side. Values: [1..500]
	uint16_t flags_is_auto_extendable : 1;	// Reads beyond max lba (num_total_blocks) return 0, not error. Writes block until bdev is extended and then succeed
	uint16_t flags_leak_fs_file_on_close : 1;	// Relevant only for bdev_type DEV_FS_FILE, deliberately leak the file on close. Used for various testing scenarios. Same can be achieved with hard linking externally so this flag is just for convenience.
	uint16_t flags_reserved           : 14;
	void clear(void) { memset(this, 0, sizeof(*this)); bdev_descriptor = -1; }
	bool is_valid(void) const { return (block_size > 0) && (bdev_descriptor > 0) && (num_max_inflight_io > 0) && (num_total_blocks > 0); }
	uint64_t get_bdev_size(void) const { return num_total_blocks * block_size; }
	void set_leak_fs_file_on_close(bool val = false) { flags_leak_fs_file_on_close = val; }
} __attribute__((aligned(sizeof(long))));

struct bdev_config_params {
	backend_bdev_id id;								// UUID of block device
	enum bdev_type {
		DUMMY_DEV_INVAL = 0x0,						// Invalid type, trap zero initialization
		DUMMY_DEV_FAIL =  'x',						// Dummy device which always fails io.    For integration testing of error/failure corner cases
		DUMMY_DEV_STUCK = 's',						// Dummy device which never completes io. For integration testing of error/failure corner cases
		DEV_FS_FILE =     'F',						// A file representing the block device.  For working with file systems and testing
		DEV_BLK_KERNEL =  'K',						// Backwards compatibility to /dev/... kernel implemented block devices including NVMe drives, /dev/zero, etc
		REMOTE_SRVR =     'N',						// Remote block device (client communicates with bdev via server)
	} type;
	enum connect_how { SHARED_RW = 'W', READ_ONLY = 'R', EXCLUSIVE_RW = 'X'} how;	// Access permissions for block device
	bool is_direct_io;								// Attempt for direct io if possible (avoid page cache)
	union connection_addr_t {						// Path/address of server/block-device. Kernel block device (like /dev/nvme0n2), Local file path (like: /tmp/my_file.txt)
		char any[64];								// Remote server address: uds/udp/tcp socket. like: /tmp/gs_uds", "t127.0.0.2" /*tcp*/, "u127.0.0.1" /*udp*/
	} conn;
	char security_cookie[16];						// 16[bytes] Utf-8 string, Used for handshake with server, to verify library is authorized to access this bdev
	bdev_config_params() { memset(this, 0, sizeof(*this)); }
	bdev_config_params(const char *_id, bdev_type t, const char* addr, const char cookie[16], bool direct, connect_how h = SHARED_RW) : type(t), how(h), is_direct_io(direct) {
		id.set_from(_id);
		strncpy(conn.any, addr, sizeof(conn) - 1);
		strncpy(security_cookie, cookie, sizeof(security_cookie) - 1);
	}
	int init_parse(int version, const char* const argv[], int argc) noexcept;	// Parse from config file
	bool is_bdev_local(void)  const { return (type == DEV_FS_FILE) || (type == DEV_BLK_KERNEL); }
	bool is_bdev_remote(void) const { return (type == REMOTE_SRVR); }
	bool has_storage(void)    const { return is_bdev_remote() || is_bdev_local(); }
	bool is_dummy(void)       const { return (type == DUMMY_DEV_FAIL) || (type == DUMMY_DEV_STUCK); }
	bool is_valid_acl(void)   const { return (how == SHARED_RW || how == READ_ONLY || how == EXCLUSIVE_RW); }
	bool is_valid(void)       const { return id.is_valid() && (has_storage() || is_dummy()) && is_valid_acl() && (conn.any[0] != 0) && (security_cookie[0] != 0); }
} __attribute__((aligned(sizeof(long))));

class client_config_file {							// You can write config file manually or auto generated using this class
	std::string c;
 public:
	SYMBOL_EXPORT client_config_file(int version = 1) noexcept  {
		c.reserve(512);								// # is remark.
		char buf[128];
		snprintf(buf, 128, "# Config file for gusli client lib\nversion=%d\n"			// First line must start with a remark, then a version
				"# bdevs: UUID-16b, type, attach_op, direct, path, security_cookie\n",	// Remark for names of fields
				version);
		c += buf;
	}
	SYMBOL_EXPORT void bdev_add(const bdev_config_params &bdev) noexcept {
		char buf[128];
		snprintf(buf, 128, "%s %c %c %c %s %s\n",		// Space separated format
			bdev.id.uuid, bdev.type, bdev.how, (bdev.is_direct_io ? 'D' : 'N'), bdev.conn.any, bdev.security_cookie);
		c += buf;
	}
	SYMBOL_EXPORT const char* get(void) const { return c.c_str(); }
};

/******************************** io request context ***********************/
enum io_type {									// Request type
	G_NOP = '0',								// Dummy op, for testing and integration. Never passed to backend
	G_READ = 'R',
	G_WRITE = 'W',
};

enum io_error_codes {							// Exhaustive list of error codes
	E_OK = 0, 									// Op success
	E_IN_TRANSFER = 			 -1,			// IO is still in execution. Returned when error code is tested too early (before IO completed).
	E_INTERNAL_FAULT = 			-11,			// Consult with developers. Internal library problem
	E_BACKEND_FAULT = 			-15,			// Underlying block device could not execute the io
	E_CANCELED_BY_CALLER = 		-10,			// IO Operation was explicitly canceled by caller.
	E_THROTTLE_RETRY_LATER = 	-12,			// IO Operation cannot be submitted
	E_PERM_FAIL_NO_RETRY = 		-13,			// IO Operation, Probably data loss, cannot execute request and no point in retrying it
	E_INVAL_PARAMS = 			-14,			// Invalid parameters/op/block-device for execution
};

struct io_buffer_t {							// For fast datapath, pre-registered io_buffers, Resembles 'struct iovec'
	/*volatile*/ void *ptr;						// Pointer to memory accessible by application and backend bdev, Source buffer for write, destination for read.
	uint64_t byte_len;							// Length of the buffer
	bool is_valid_for(uint64_t block_size) const {
		const uint64_t align = (byte_len | (uint64_t)ptr);
		return !((align % block_size) || (ptr == nullptr) || (byte_len == 0));
	}
	const  io_buffer_t&     init(void* p, uint64_t len) { ptr = p; byte_len = len; return *this; }
	static io_buffer_t construct(void* p, uint64_t len) { io_buffer_t rv; return rv.init(p, len); }
} __attribute__((aligned(sizeof(long))));

struct io_map_t {								// IO mapping of data buffer to block device lba
	io_buffer_t data;
	uint64_t offset_lba_bytes;					// Starting offset of the io on blockdevice, all address must be aligned to blocks.
	const io_map_t &init(void* ptr, uint64_t len, uint64_t offset) { data.init(ptr, len); offset_lba_bytes = offset; return *this; }
	bool is_valid_for(uint64_t block_size) const { return data.is_valid_for(block_size) && ((offset_lba_bytes % block_size) == 0); }
	uint64_t get_offset_end_lba(void) const { return (offset_lba_bytes + data.byte_len); }
} __attribute__((aligned(sizeof(long))));

struct io_multi_map_t {							// Scatter gather list of multi range io, 8[b] header followed by array of entries.
	uint32_t n_entries;							// Number of elements in the array > 1, typically 2 - 10K entries
	uint32_t _reserved;							// Special private cookie to encode version and detect wrong client/srvr mapping. Dont touch
	io_map_t entries[0];
	uint64_t  my_size(void) const { return sizeof(*this) + n_entries*sizeof(entries[0]); }
	uint64_t buf_size(void) const { uint64_t rv = 0; for (uint32_t i = 0; i < n_entries; i++) rv += entries[i].data.byte_len; return rv; }
	bool is_valid(void) const { return (n_entries > 1) && (_reserved == 0x6f696d6d); } // For 0,1 range, multi map is not needed
	bool init_num_entries(uint32_t n) { n_entries = n; _reserved = 0x6f696d6d; return is_valid(); }
} __attribute__((aligned(sizeof(long))));

class io_request_base {							// Data structure for issuing IO
 public:
	class params_t {							// Parameters for IO, Use setter/getter functions to initialize them
		io_map_t _map;							// Holds a mapping for IO buffer or a mapping to scatter-gather list of mappings
		int32_t _bd_id;							// Take from bdev_info::bdev_descriptor. Identifies the opened block device to which the io is sent
		//uint32_t timeout_msec;				// Optional timeout for IO in [msec].  0 or ~0 mean infinity. Not supported, Use async io mode and cancel the io if needed
		enum io_type _op         : 8;			// Operation to be performed
		uint8_t _priority        : 7;			// [0..100] priority. 100 is highest, 0 lowest. Default = 0
		uint8_t _is_mutable_data : 1;			// Default false, Set to true if content of io buffer might be changed by caller while IO is in air. If cannot guarantee immutability io will suffer a penalty of internal copy of the buffer
		uint16_t _assume_safe_io : 1;			// Default false, Set to true if caller verifies correctness of io (fully inside mapped area, etc...), Skips internal checks so IO uses less client side CPU
		uint16_t _try_using_uring_api : 1;		// If possible use uring api, more efficient for io's with large amount of ranges, but does not run in default containers of kubernetes due to security issues of lib-uring
		uint16_t _has_mm              : 1;		// First 4K of io buffer contains io_multi_map_t (scatter gather) description for multi-io
		uint16_t _async_no_comp       : 1;		// Internal flag, IO is async but caller will poll it instead of completion
		uint16_t _is_remote_bdev      : 1;		// Internal flag, IO is going to server (not local client block device)
		uint16_t _unique_id           : 11;		// Internal use, During execution each IO gets a unique id (unique for in air io's) so it can be easily tracked and retrieved if stuck
		void (*_comp_cb)(void* ctx);			// Completion callback, Called from library internal thread, dont do processing/wait-for-locks in this context!
		void *_comp_ctx;						// Callers Completion context passed to the function above
		friend class backend_io_req;			// Access to _comp fields
		friend class server_io_req;
	 public:
		// API to initialize io params
		template<class C, typename F> void set_completion(C ctx, F cb) { _comp_ctx = (void*)ctx, _comp_cb = (void (*)(void*))cb; _async_no_comp = false; }
									  void set_blocking(void) {          _comp_ctx = NULL;       _comp_cb = NULL;                _async_no_comp = false; }
									  void set_async_pollable(void) {    _comp_ctx = NULL;       _comp_cb = NULL;                _async_no_comp = true; }
		void init_1_rng(enum io_type op, int id, uint64_t lba, uint64_t len, void *buf) { _has_mm = 0; _op = op; _bd_id = id; _map.init(buf,        len,          lba); }
		void init_multi(enum io_type op, int id,  const io_multi_map_t& mm) {             _has_mm = 1; _op = op; _bd_id = id; _map.init((void*)&mm, mm.my_size(), 0  ); }
		params_t &set_mutable_data(      bool v) { _is_mutable_data = v;     return *this; }	// Set, when content of io buffer might be changed by caller while IO is in air. If cannot guarantee immutability io will suffer a penalty of internal copy of the buffer
		params_t &set_try_use_uring(     bool v) { _try_using_uring_api = v; return *this; }
		params_t &set_safe_io(           bool v) { _assume_safe_io = v;      return *this; }
		params_t &set_priority(uint8_t percents) { _priority = percents; if (_priority > 100) _priority = 100;  return *this; }

		// API to change initialized io params, for resubmission
		params_t &set(enum io_type op) { _op = op;   return *this; }	// Example: You issued write and immediately want to issue read with the same params
		params_t &set_dev(int32_t bd) { _bd_id = bd; return *this; }	// Example: Issue write to disk0 and afterwards to disk1 for backup
		io_map_t &change_map(void)    { return _map; }					// Example: Wrote io to offset 0x40 and for backup want to write the same io to offset 0x140 as well.change_map().offset_lba_bytes += 0x100;

		// API to get io params that you already set
		enum io_type op(void)             const { return _op; }
		bool is_multi_range(void)         const { return _has_mm; }
		uint64_t buf_size(void)           const { return (is_multi_range() ? ((const io_multi_map_t*)_map.data.ptr)->buf_size() : _map.data.byte_len); }
		uint32_t num_ranges(void)         const { return (is_multi_range() ? ((const io_multi_map_t*)_map.data.ptr)->n_entries  : 1); }
		const io_map_t &map(void)         const { return _map; }
		int32_t get_bdev_descriptor(void) const { return _bd_id; }
		bool is_safe_io(void)             const { return _assume_safe_io; }
		bool may_use_uring(void)          const { return _try_using_uring_api; }
		bool has_callback(void)           const { return _comp_cb != NULL; }
		bool is_polling_mode(void)        const { return _async_no_comp; }
		bool is_blocking_io(void)         const { return !has_callback() && !is_polling_mode(); }
	} params;
	io_request_base() { memset(this, 0, sizeof(*this)); }
	SYMBOL_EXPORT void submit_io(void) noexcept;						// Execute io. May Call again to retry failed io. All errors/success should be checked with function below
	SYMBOL_EXPORT enum io_error_codes get_error(void) noexcept;			// Query io completion status for blocking IO, poll on pollable io. Running on async callback io may yield racy results
	enum cancel_rv { G_CANCELED = 'V', G_ALLREADY_DONE = 'D' };			// DONE = IO finished error/success. CANCELED = Successfully canceled (Async IO, completion will not be executed)
	SYMBOL_EXPORT_NO_DISCARD enum cancel_rv cancel_wait(void) noexcept;	// Cancel I/O request. For Async IO, completion will not arrive after call to this function, but non careful user may call it while completion callback is concurrently running. Note: IO cancellation blocks until registered memory will not be used anymore
	SYMBOL_EXPORT void done(void) noexcept;								// Must call done() after you finish analyzing the run of submit_io(). Can submit the io again / free it / change it and submit again after call to this function
 protected:																// Below extra 16[b] for execution state
	class io_request_executor_base* _exec;								// During execution executor attaches to IO, Server side uses it to execute io
	struct output_t { int64_t rv; } out;								// Negative error code or amount of bytes transferred.
};

class io_request : public io_request_base {								// Example how you can inherit from io base class or use this class for your io
 public:
	SYMBOL_EXPORT ~io_request();										// Added destructor that verifies io was properly finished/done (not still running / etc). You can use the base class if you dont need this extra protection
};

/******************************** Global library context ***********************/
// API for global client library context (library singleton)
enum connect_rv { C_OK = 0, C_NO_DEVICE = -100,
	C_NO_RESPONSE /* Device exists no response from backend*/,
	C_REMAINS_OPEN /* Already open / Cannot close because in use*/,
	C_WRONG_ARGUMENTS};

static constexpr const char* thread_names_prefix = "gusli_";		// All library aux threads will have this prefix for easier ps | grep

class clnt_init_exception : public std::runtime_error {				// Initialization exception thrown during construction error
 public:
	clnt_init_exception(int code, const std::string& msg) : std::runtime_error(msg), error_code_(code) {}
	int code(void) const noexcept { return error_code_; }
 private:
	int error_code_;
};

struct no_implicit_constructors {	// No copy/move/assign operations to prevent accidental copying of resources, leaks, double free and performance degradation
	no_implicit_constructors(           const no_implicit_constructors& ) = delete;
	no_implicit_constructors(                 no_implicit_constructors&&) = delete;
	no_implicit_constructors& operator=(const no_implicit_constructors& ) = delete;
	no_implicit_constructors& operator=(      no_implicit_constructors&&) = delete;
	no_implicit_constructors() {}
};

class global_clnt_context : no_implicit_constructors {		// RAII (Resource Acquisition Is Initialization) to manage the singleton lifecycle
	// Note: this is an empty class, as implementation is an obscures singleton. Pass it by reference to helper functions
 public:
	struct init_params {							// All params are optional
		FILE* log = stdout;							// Redirect logs of the library to this file (must be already properly opened)
		const char* config_file = NULL;				// Path to config file (like "./cfg.txt") or content of config file generated with client_config_file::get()
		const char* client_name = NULL;				// For debug, client identifier
		unsigned int max_num_simultaneous_requests = 256;
	};
	static constexpr const int BREAKING_VERSION = 1;					// Hopefully will always be 1. When braking API change is introduced, this version goes up so apps which link with the library can detect that during compilation
	SYMBOL_EXPORT global_clnt_context(const init_params& par);			// Throws clnt_init_exception upon error. Use string and error code to find out more about failure
	SYMBOL_EXPORT ~global_clnt_context() noexcept;
	SYMBOL_EXPORT const char *get_metadata_json(void) const noexcept;	// Get the version of the library to adapt application dynamically to library features set.

	// Open/Close block device and register memory
	using mem_list = std::vector<io_buffer_t>;
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv bdev_connect(      const backend_bdev_id&)                  const noexcept;	// Open block device, must be done before register buffers or submitting io
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv bdev_bufs_register(const backend_bdev_id&, const mem_list&) const noexcept;	// Register shared memory buffers which will store the content of future io
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv bdev_bufs_unregist(const backend_bdev_id&, const mem_list&) const noexcept;
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv bdev_disconnect(   const backend_bdev_id&)                  const noexcept;

	// Same as above but with implicit open/close using auto refcount on bdev
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv open__bufs_register(const backend_bdev_id&, const mem_list&)                           const noexcept;	// Register shared memory buffers which will store the content of future io
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv close_bufs_unregist(const backend_bdev_id&, const mem_list&, bool stop_server = false) const noexcept;	// Reverse of the above, stop server param kills the server app. No reconnection is possible

	// Get block device information (after it was opened)
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv bdev_get_info(      const backend_bdev_id&, bdev_info &rv)  const noexcept;
	SYMBOL_EXPORT_NO_DISCARD int32_t   bdev_get_descriptor(      const backend_bdev_id&)                 const noexcept;

	// Advanced Control API towards server, for debugging / testing / crisis management
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv bdev_override_info( const backend_bdev_id&, const bdev_info &val) noexcept;
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv bdev_force_close(   const backend_bdev_id&, bool reconnect = false) const noexcept;	// Call this if you want to unregister-mem / close block device but some IO's are stuck. Cancels all io air IO's, executes their callbacks. Can Reconnect to server and reregister memory buffers
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv bdev_force_refresh( const backend_bdev_id& id)              const noexcept { return bdev_force_close(id, true); }
	SYMBOL_EXPORT         void bdev_ctl_report_data_corruption(  const backend_bdev_id&, uint64_t lba)   const noexcept;	// Hopefully never use: Report data corruption at lba[bytes]. Will kill the server to avoid further data corruption.
	SYMBOL_EXPORT     uint32_t bdev_ctl_get_num_in_air_ios(      const backend_bdev_id&)                 const noexcept;	// Get the amount of ios in air for this block device. For debugging
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv bdev_ctl_log_msg(   const backend_bdev_id&, const std::string &s) const noexcept;	// Send a short message to write to server logs. Max 56 bytes/ascii-characters
	SYMBOL_EXPORT_NO_DISCARD enum connect_rv bdev_ctl_reboot(    const backend_bdev_id&, const std::string &s) const noexcept;	// Will force a server to disconnect and reconnect a client, pass optional debug reason
 private:
	static constexpr const char* metadata_json_format = "{\"%s\":{\"version\" : \"%s\", \"commit\" : \"%lx\", \"optimization\" : \"%s\", \"trace_level\" : %u, \"Build\" : \"%s\"}}";
};

} // namespace