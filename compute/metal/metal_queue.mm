/*
 *  Flo's Open libRary (floor)
 *  Copyright (C) 2004 - 2022 Florian Ziesche
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License only.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <floor/compute/metal/metal_queue.hpp>

#if !defined(FLOOR_NO_METAL)

#include <floor/darwin/darwin_helper.hpp>
#include <floor/compute/metal/metal_indirect_command.hpp>

// make GPUStartTime/GPUEndTime available everywhere
@protocol MTLCommandBufferProfiling <MTLCommandBuffer>
@property(readonly) double GPUEndTime;
@property(readonly) double GPUStartTime;
@end

metal_queue::metal_queue(const compute_device& device_, id <MTLCommandQueue> queue_) : compute_queue(device_), queue(queue_) {
	// check if we can do profiling
	id <MTLCommandBuffer> buffer = [queue commandBufferWithUnretainedReferences];
	__unsafe_unretained id <MTLCommandBufferProfiling> prof_buffer = (id <MTLCommandBufferProfiling>)buffer;
	if ([prof_buffer respondsToSelector:@selector(GPUStartTime)] &&
		[prof_buffer respondsToSelector:@selector(GPUEndTime)]) {
		can_do_profiling = true;
	}
	buffer = nil;
}

void metal_queue::finish() const {
	// need to copy current set of command buffers, so we don't deadlock when removing completed command buffers
	decltype(cmd_buffers) cur_cmd_buffers;
	{
		GUARD(cmd_buffers_lock);
		cur_cmd_buffers = cmd_buffers;
	}
	for(const auto& cmd_buffer : cur_cmd_buffers) {
		[cmd_buffer.first waitUntilCompleted];
	}
}

void metal_queue::flush() const {
	// need to copy current set of command buffers, so we don't deadlock when removing completed command buffers
	decltype(cmd_buffers) cur_cmd_buffers;
	{
		GUARD(cmd_buffers_lock);
		cur_cmd_buffers = cmd_buffers;
	}
	for(const auto& cmd_buffer : cur_cmd_buffers) {
		[cmd_buffer.first waitUntilScheduled];
	}
}

void metal_queue::execute_indirect(const indirect_command_pipeline& indirect_cmd,
								   const uint32_t command_offset,
								   const uint32_t command_count) const {
	if (command_count == 0) {
		return;
	}
	
	const auto& mtl_indirect_cmd = (const metal_indirect_command_pipeline&)indirect_cmd;
	const auto mtl_indirect_pipeline_entry = mtl_indirect_cmd.get_metal_pipeline_entry(device);
	if (!mtl_indirect_pipeline_entry) {
		log_error("no indirect command pipeline state for device \"$\" in indirect command pipeline \"$\"",
				  device.name, indirect_cmd.get_description().debug_label);
		return;
	}
	
	const auto range = mtl_indirect_cmd.compute_and_validate_command_range(command_offset, command_count);
	if (!range) {
		return;
	}
	
	// create and setup the compute encoder
	id <MTLCommandBuffer> cmd_buffer = make_command_buffer();
	id <MTLComputeCommandEncoder> encoder = [cmd_buffer computeCommandEncoder];
	
	// declare all used resources
	// TODO: efficient resource usage declaration for command ranges != full range (see warning above)
	const auto& resources = mtl_indirect_pipeline_entry->get_resources();
	if (!resources.read_only.empty()) {
		[encoder useResources:resources.read_only.data()
						count:resources.read_only.size()
						usage:MTLResourceUsageRead];
	}
	if (!resources.write_only.empty()) {
		[encoder useResources:resources.write_only.data()
						count:resources.write_only.size()
						usage:MTLResourceUsageWrite];
	}
	if (!resources.read_write.empty()) {
		[encoder useResources:resources.read_write.data()
						count:resources.read_write.size()
						usage:(MTLResourceUsageRead | MTLResourceUsageWrite)];
	}
	if (!resources.read_only_images.empty()) {
		[encoder useResources:resources.read_only_images.data()
						count:resources.read_only_images.size()
						usage:(MTLResourceUsageRead | MTLResourceUsageSample)];
	}
	if (!resources.read_write_images.empty()) {
		[encoder useResources:resources.read_write_images.data()
						count:resources.read_write_images.size()
						usage:(MTLResourceUsageRead | MTLResourceUsageWrite | MTLResourceUsageSample)];
	}
	
	[encoder executeCommandsInBuffer:mtl_indirect_pipeline_entry->icb
						   withRange:*range];
	
	[encoder endEncoding];
	[cmd_buffer commit];
}

const void* metal_queue::get_queue_ptr() const {
	return (__bridge const void*)queue;
}

void* metal_queue::get_queue_ptr() {
	return (__bridge void*)queue;
}

id <MTLCommandQueue> metal_queue::get_queue() const {
	return queue;
}

id <MTLCommandBuffer> metal_queue::make_command_buffer() const {
	id <MTLCommandBuffer> cmd_buffer = [queue commandBufferWithUnretainedReferences];
	[cmd_buffer addCompletedHandler:^(id <MTLCommandBuffer> buffer) {
		GUARD(cmd_buffers_lock);
		const auto iter = find_if(cbegin(cmd_buffers), cend(cmd_buffers), [&buffer](const decltype(cmd_buffers)::value_type& elem) {
			return (elem.first == buffer);
		});
		if(iter == cend(cmd_buffers)) {
			log_error("failed to find metal command buffer $X!", buffer);
			return;
		}
		
		if (iter->second && can_do_profiling) {
			__unsafe_unretained id <MTLCommandBufferProfiling> prof_buffer = (id <MTLCommandBufferProfiling>)buffer;
			const auto elapsed_time = ([prof_buffer GPUEndTime] - [prof_buffer GPUStartTime]);
			profiling_sum += uint64_t(elapsed_time * 1000000.0);
		}
		// else: nothing to do here
		
		cmd_buffers.erase(iter);
	}];
	{
		GUARD(cmd_buffers_lock);
		cmd_buffers.emplace_back(cmd_buffer, is_profiling);
	}
	return cmd_buffer;
}

void metal_queue::start_profiling() {
	if (!can_do_profiling) {
		// fallback to host side profiling
		compute_queue::start_profiling();
		return;
	}
	
	// signal new buffers that we're profiling
	is_profiling = true;
}

uint64_t metal_queue::stop_profiling() {
	if (!can_do_profiling) {
		// fallback to host side profiling
		return compute_queue::stop_profiling();
	}
	
	// wait on all buffers
	finish();
	
	const uint64_t ret = profiling_sum;
	
	// clear
	profiling_sum = 0;
	is_profiling = false;
	
	return ret;
}

#endif
