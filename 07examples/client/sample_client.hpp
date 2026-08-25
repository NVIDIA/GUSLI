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
#pragma once
#include "07examples/client/io_submittion_example.hpp"

/*****************************************************************************/
#define n_block(i) (info.block_size * (i))
#define mappend_block(p, i) ((void*)((uint64_t)(p) + n_block(i)))
int client_test_write_read_verify_1blk(const gusli::bdev_info& info, unitest_io &my_io, const uint64_t lba) {
	char *user_buf = my_io.io_buf + n_block(3);		// We use a short io, so take offset in io buffer, to test address translation
	my_io.io.params.init_1_rng(gusli::G_NOP, info.bdev_descriptor, lba, n_block(2), user_buf);
	const gusli::io_map_t& map = my_io.io.params.map();
	test_lba::map1_print(map, "clnt");
	test_lba::map1_fill( map, info.block_size);
	my_io.exec(gusli::G_WRITE, io_exec_mode::ASYNC_CB);
	for_each_exec_mode(i) {
		my_io.clean_buf();
		my_io.exec(gusli::G_READ, (io_exec_mode)i);
		my_assert(map.data.ptr == user_buf);
		test_lba::map1_verify_and_clean(map, info.block_size);
	}
	return 0;
}

int client_test_write_read_verify_multi(const gusli::bdev_info& info, const std::vector<gusli::io_buffer_t> &io_bufs) {
	unitest_io my_io;
	const gusli::io_buffer_t& map = io_bufs[0];
	const uint64_t lbas[5] = {n_block(0x01B), n_block(0x21), n_block(0x63), n_block(0x52), n_block(0x51)};
	/* IO of 7 ram blocks, 3 ranges: {range0=2[blk], sgl=1[blk], range1=3[blk], range2=1[blk]}
		Block   |   0   |   1   |   2   |   3   |   4   |   5   |   6    |
		--------+-------+-------|---------------+-------+-------+--------|
		Content |     Range0    |  sgl  |         Range1        | Range2 |
		LBA     |     lbas[0]   |   2   |         lbas[1]       | lbas[2]|
	*/
	my_assert(map.byte_len >= info.block_size * 7);
	gusli::io_multi_map_t* mio = (gusli::io_multi_map_t*)mappend_block(map.ptr, 2);			// Scatter gather in third block
	if (true) {
		mio->init_num_entries(3);
		mio->entries[0].init(mappend_block(map.ptr, 0), n_block(2), lbas[0]);
		mio->entries[1].init(mappend_block(map.ptr, 3), n_block(3), lbas[1]);
		mio->entries[2].init(mappend_block(map.ptr, 6), n_block(1), lbas[2]);
	}
	if (io_bufs.size() > 1) {
		const gusli::io_buffer_t& ext = io_bufs[1];
		/* + 3 block to IO (first and last in second range): {range3=2[blk], range4=1[blk]}
			Block   |   0    |   1   |    ......     | last-1 |  last  |
			--------+--------+-------|---------------+--------+--------|
			Content | Range4 |       |               |      Range3     |
			LBA     | lbas[4]|       |               |      lbas[3]    |
		*/
		my_assert(ext.byte_len >= info.block_size * 3);
		const uint64_t l = (ext.byte_len / info.block_size) - 2;				// Last 2 block in mapped area
		mio->init_num_entries(mio->n_entries + 2);
		mio->entries[3].init(mappend_block(ext.ptr, l), n_block(2), lbas[3]);
		mio->entries[4].init(mappend_block(ext.ptr, 0), n_block(1), lbas[4]);
	}
	my_assert(mio->my_size() < info.block_size);					// There is enough space in a block to fit sgl
	my_io.io.params.init_multi(gusli::G_NOP, info.bdev_descriptor, *mio);
	test_lba::mmio_print(mio, "clnt");
	test_lba::mmio_fill( mio, info.block_size);
	my_io.exec(gusli::G_WRITE, io_exec_mode::ASYNC_CB);
	test_lba::mmio_verify_and_clean(mio, info.block_size);
	my_io.exec(gusli::G_READ , io_exec_mode::ASYNC_CB);
	test_lba::mmio_verify_and_clean(mio, info.block_size);
	my_io.exec(gusli::G_READ , io_exec_mode::SYNC_BLOCKING_1_BY_1);	// Also verify blocking read
	test_lba::mmio_verify_and_clean(mio, info.block_size);
	my_io.expect_success(true).exec(gusli::G_READ, io_exec_mode::POLLABLE);	// Pollable-Fails - not supported yet
	return 0;
}

/* Simple test of client process: Connect to server, Write IO and verify it
   with read. Do 1 range and multi-range io. Stop the server at the end */
int client_simple_test_of_server(const char* clnt_name, const int n_devs, const char* const bdev_uuid[], const char* const srvr_addr[]) {
	gusli::global_clnt_context::init_params p;
	p.client_name = clnt_name;
	p.max_num_simultaneous_requests = MAX_SERVER_IN_FLIGHT_IO;
	gusli::client_config_file conf(1 /*Version*/);
	{	// Generate config
		using gsc = gusli::bdev_config_params;
		for (int b = 0; b < n_devs; b++)
			conf.bdev_add(gsc(bdev_uuid[b], gsc::bdev_type::REMOTE_SRVR, srvr_addr[b],  "sec=0x04", true, gsc::connect_how::SHARED_RW));
		p.config_file = conf.get();
	}
	gusli::global_clnt_context gc(p);
	log_unitest("Client metadata= %s\n", gc.get_metadata_json());
	// Create io buffers
	unitest_io my_io;
	std::vector<gusli::io_buffer_t> io_bufs;
	io_bufs.emplace_back(my_io.get_map());

	// Map io buffers for read/write operations and auto open block devices
	for (int b = 0; b < n_devs; b++) {
		log_line("Remote server %s, connecting...", srvr_addr[b]);
		struct gusli::backend_bdev_id bdev; bdev.set_from(bdev_uuid[b]);
		int n_attempts = 0;		// Try to connect for 5 seconds
		enum gusli::connect_rv con_rv = gusli::connect_rv::C_NO_RESPONSE;
		for (; ((con_rv == gusli::connect_rv::C_NO_RESPONSE) && (n_attempts < 10)); n_attempts++ ) {
			con_rv = gc.open__bufs_register(bdev, io_bufs);
			std::this_thread::sleep_for(std::chrono::milliseconds(500));	// Wait for servers to be up
		}
		if (con_rv != gusli::connect_rv::C_OK) {
			log_uni_failure("Unable to connect to server %s(%s) after %d attempts. rv=%d. Aborting test\n", bdev_uuid[b], srvr_addr[b], n_attempts, con_rv);
			return -ENODEV;
		}
		gusli::bdev_info info;
		my_assert(gc.bdev_get_info(bdev, info) == gusli::connect_rv::C_OK);
		my_assert(info.num_total_blocks > 0x100);			// We write to first few blocks
		log_line("Remote server %s opened and mapped bufs", info.name);
	}

	for (int b = 0; b < n_devs; b++) {
		log_line("%s: Write->Read test of 1[blk]", srvr_addr[b]);
		struct gusli::backend_bdev_id bdev; bdev.set_from(bdev_uuid[b]);
		gusli::bdev_info info;
		my_assert(gc.bdev_get_info(bdev, info) == gusli::connect_rv::C_OK);
		client_test_write_read_verify_1blk(info, my_io, (b * info.block_size));
	}
	for (int b = 0; b < n_devs; b++) {
		struct gusli::backend_bdev_id bdev; bdev.set_from(bdev_uuid[b]);
		gusli::bdev_info info;
		my_assert(gc.bdev_get_info(bdev, info) == gusli::connect_rv::C_OK);
		log_line("%s: IO-to-srvr-multi-range N[blks] + 1sg block", srvr_addr[b]);
		client_test_write_read_verify_multi(info, io_bufs);
	}
	const bool kill_server = true;
	for (int b = 0; b < n_devs; b++) {
		log_line("%s: Disconnect & Kill", srvr_addr[b]);
		struct gusli::backend_bdev_id bdev; bdev.set_from(bdev_uuid[b]);
		my_assert(gc.close_bufs_unregist(bdev, io_bufs, kill_server) == gusli::connect_rv::C_OK);
	}
	log_uni_success("Client: Test OK\n");
	return 0;
}

