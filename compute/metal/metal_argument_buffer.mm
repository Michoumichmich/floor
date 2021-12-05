/*
 *  Flo's Open libRary (floor)
 *  Copyright (C) 2004 - 2021 Florian Ziesche
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

#include <floor/core/essentials.hpp>

#if !defined(FLOOR_NO_METAL)
#include <floor/compute/metal/metal_argument_buffer.hpp>
#include <floor/compute/metal/metal_args.hpp>

metal_argument_buffer::metal_argument_buffer(const compute_kernel& func_, shared_ptr<compute_buffer> storage_buffer_,
											 aligned_ptr<uint8_t>&& storage_buffer_backing_,
											 id <MTLArgumentEncoder> encoder_, const llvm_toolchain::function_info& arg_info_,
											 vector<uint32_t>&& arg_indices_) :
argument_buffer(func_, storage_buffer_), storage_buffer_backing(move(storage_buffer_backing_)), encoder(encoder_), arg_info(arg_info_),
arg_indices(move(arg_indices_)) {}

static void sort_and_unique_resources(vector<id <MTLResource> __unsafe_unretained>& res) {
	std::sort(res.begin(), res.end());
	auto last_iter = std::unique(res.begin(), res.end());
	if (last_iter != res.end()) {
		res.erase(last_iter, res.end());
	}
}

void metal_argument_buffer::set_arguments(const compute_queue& dev_queue [[maybe_unused]], const vector<compute_kernel_arg>& args) {
	auto mtl_storage_buffer = (metal_buffer*)storage_buffer.get();
	auto mtl_buffer = mtl_storage_buffer->get_metal_buffer();
	
	[encoder setArgumentBuffer:mtl_buffer offset:0];
	assert(&dev_queue.get_device() == &mtl_storage_buffer->get_device());
	resources = {}; // clear current resource tracking
	metal_args::set_and_handle_arguments<metal_args::ENCODER_TYPE::ARGUMENT>(mtl_storage_buffer->get_device(), encoder, { &arg_info }, args, {},
																			 (!arg_indices.empty() ? &arg_indices : nullptr),
																			 &resources);
	
	sort_and_unique_resources(resources.read_only);
	sort_and_unique_resources(resources.write_only);
	sort_and_unique_resources(resources.read_write);
	sort_and_unique_resources(resources.read_only_images);
	sort_and_unique_resources(resources.read_write_images);
	
#if !defined(FLOOR_IOS)
	// signal buffer update if this is a managed buffer
	if ((mtl_storage_buffer->get_metal_resource_options() & MTLResourceStorageModeMask) == MTLResourceStorageModeManaged) {
		const auto update_range = (uint64_t)[encoder encodedLength];
		[mtl_buffer didModifyRange:NSRange { 0, update_range }];
	}
#endif
}

void metal_argument_buffer::make_resident(id <MTLComputeCommandEncoder> enc) const {
	if (!resources.read_only.empty()) {
		[enc useResources:resources.read_only.data()
					count:resources.read_only.size()
					usage:MTLResourceUsageRead];
	}
	if (!resources.write_only.empty()) {
		[enc useResources:resources.write_only.data()
					count:resources.write_only.size()
					usage:MTLResourceUsageWrite];
	}
	if (!resources.read_write.empty()) {
		[enc useResources:resources.read_write.data()
					count:resources.read_write.size()
					usage:(MTLResourceUsageRead | MTLResourceUsageWrite)];
	}
	if (!resources.read_only_images.empty()) {
		[enc useResources:resources.read_write.data()
					count:resources.read_write.size()
					usage:(MTLResourceUsageRead | MTLResourceUsageSample)];
	}
	if (!resources.read_write_images.empty()) {
		[enc useResources:resources.read_write_images.data()
					count:resources.read_write_images.size()
					usage:(MTLResourceUsageRead | MTLResourceUsageWrite | MTLResourceUsageSample)];
	}
}

void metal_argument_buffer::make_resident(id <MTLRenderCommandEncoder> enc, const llvm_toolchain::FUNCTION_TYPE& func_type) const {
	assert(func_type == llvm_toolchain::FUNCTION_TYPE::VERTEX || func_type == llvm_toolchain::FUNCTION_TYPE::FRAGMENT);
	if ([encoder respondsToSelector:@selector(useResources:count:usage:stages:)]) {
		const auto stage = (func_type == llvm_toolchain::FUNCTION_TYPE::VERTEX ? MTLRenderStageVertex : MTLRenderStageFragment);
		if (!resources.read_only.empty()) {
			[enc useResources:resources.read_only.data()
						count:resources.read_only.size()
						usage:MTLResourceUsageRead
					   stages:stage];
		}
		if (!resources.write_only.empty()) {
			[enc useResources:resources.write_only.data()
						count:resources.write_only.size()
						usage:MTLResourceUsageWrite
					   stages:stage];
		}
		if (!resources.read_write.empty()) {
			[enc useResources:resources.read_write.data()
						count:resources.read_write.size()
						usage:(MTLResourceUsageRead | MTLResourceUsageWrite)
					   stages:stage];
		}
		if (!resources.read_only_images.empty()) {
			[enc useResources:resources.read_write.data()
						count:resources.read_write.size()
						usage:(MTLResourceUsageRead | MTLResourceUsageSample)
					   stages:stage];
		}
		if (!resources.read_write_images.empty()) {
			[enc useResources:resources.read_write_images.data()
						count:resources.read_write_images.size()
						usage:(MTLResourceUsageRead | MTLResourceUsageWrite | MTLResourceUsageSample)
					   stages:stage];
		}
	} else {
		if (!resources.read_only.empty()) {
			[enc useResources:resources.read_only.data()
						count:resources.read_only.size()
						usage:MTLResourceUsageRead];
		}
		if (!resources.write_only.empty()) {
			[enc useResources:resources.write_only.data()
						count:resources.write_only.size()
						usage:MTLResourceUsageWrite];
		}
		if (!resources.read_write.empty()) {
			[enc useResources:resources.read_write.data()
						count:resources.read_write.size()
						usage:(MTLResourceUsageRead | MTLResourceUsageWrite)];
		}
		if (!resources.read_only_images.empty()) {
			[enc useResources:resources.read_write.data()
						count:resources.read_write.size()
						usage:(MTLResourceUsageRead | MTLResourceUsageSample)];
		}
		if (!resources.read_write_images.empty()) {
			[enc useResources:resources.read_write_images.data()
						count:resources.read_write_images.size()
						usage:(MTLResourceUsageRead | MTLResourceUsageWrite | MTLResourceUsageSample)];
		}
	}
}

#endif
