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

#include <floor/core/essentials.hpp>

#if !defined(FLOOR_NO_METAL)
#include <floor/compute/metal/metal_compute.hpp>
#include <floor/core/platform.hpp>
#include <floor/core/gl_support.hpp>
#include <floor/core/logger.hpp>
#include <floor/core/core.hpp>
#include <floor/core/file_io.hpp>

#if defined(__APPLE__)
#include <floor/darwin/darwin_helper.hpp>
#endif

#include <floor/compute/llvm_toolchain.hpp>
#include <floor/compute/universal_binary.hpp>
#include <floor/compute/metal/metal_buffer.hpp>
#include <floor/compute/metal/metal_image.hpp>
#include <floor/compute/metal/metal_device.hpp>
#include <floor/compute/metal/metal_device_query.hpp>
#include <floor/compute/metal/metal_program.hpp>
#include <floor/compute/metal/metal_queue.hpp>
#include <floor/compute/metal/metal_indirect_command.hpp>
#include <floor/graphics/metal/metal_pipeline.hpp>
#include <floor/graphics/metal/metal_pass.hpp>
#include <floor/graphics/metal/metal_renderer.hpp>
#include <floor/vr/vr_context.hpp>
#include <floor/floor/floor.hpp>
#include <Metal/Metal.h>
#include <Metal/MTLCaptureScope.h>

// include again to clean up macro mess
#include <floor/core/essentials.hpp>

// only need this on os x right now
#if !defined(FLOOR_IOS)

@protocol MTLDeviceSPI <MTLDevice>
@property(readonly) unsigned long long dedicatedMemorySize;
@property(readonly) unsigned long long iosurfaceTextureAlignmentBytes;
@property(readonly) unsigned long long linearTextureAlignmentBytes;
@property(readonly) unsigned long long maxTextureLayers;
@property(readonly) unsigned long long maxTextureDimensionCube;
@property(readonly) unsigned long long maxTextureDepth3D;
@property(readonly) unsigned long long maxTextureHeight3D;
@property(readonly) unsigned long long maxTextureWidth3D;
@property(readonly) unsigned long long maxTextureHeight2D;
@property(readonly) unsigned long long maxTextureWidth2D;
@property(readonly) unsigned long long maxTextureWidth1D;
@property(readonly) unsigned long long minBufferNoCopyAlignmentBytes;
@property(readonly) unsigned long long minConstantBufferAlignmentBytes;
@property(readonly) unsigned long long maxVisibilityQueryOffset;
@property(readonly) float maxPointSize;
@property(readonly) float maxLineWidth;
@property(readonly) unsigned long long maxComputeThreadgroupMemory;
@property(readonly) unsigned long long maxTotalComputeThreadsPerThreadgroup;
@property(readonly) unsigned long long maxComputeLocalMemorySizes;
@property(readonly) unsigned long long maxComputeInlineDataSize;
@property(readonly) unsigned long long maxComputeSamplers;
@property(readonly) unsigned long long maxComputeTextures;
@property(readonly) unsigned long long maxComputeBuffers;
@property(readonly) unsigned long long maxFragmentInlineDataSize;
@property(readonly) unsigned long long maxFragmentSamplers;
@property(readonly) unsigned long long maxFragmentTextures;
@property(readonly) unsigned long long maxFragmentBuffers;
@property(readonly) unsigned long long maxInterpolants;
@property(readonly) unsigned long long maxVertexInlineDataSize;
@property(readonly) unsigned long long maxVertexSamplers;
@property(readonly) unsigned long long maxVertexTextures;
@property(readonly) unsigned long long maxVertexBuffers;
@property(readonly) unsigned long long maxVertexAttributes;
@property(readonly) unsigned long long maxColorAttachments;
@property(readonly) unsigned long long featureProfile;
@property(nonatomic) BOOL metalAssertionsEnabled;
@property(readonly) unsigned long long doubleFPConfig;
@property(readonly) unsigned long long singleFPConfig;
@property(readonly) unsigned long long halfFPConfig;
@property(readonly, getter=isMagicMipmapSupported) BOOL magicMipmapSupported;
//@property(readonly) int llvmVersion;
@property(readonly) unsigned long long recommendedMaxWorkingSetSize;
@property(readonly) unsigned long long registryID;
@property(readonly) BOOL requiresIABEmulation;
@property(readonly) BOOL supportPriorityBand;

@optional
- (NSString *)productName;
- (NSString *)familyName;
- (NSString *)vendorName;
@end

#endif

metal_compute::metal_compute(const bool enable_renderer_,
							 vr_context* vr_ctx_,
							 const vector<string> whitelist) :
compute_context(), vr_ctx(vr_ctx_), enable_renderer(enable_renderer_) {
#if defined(FLOOR_IOS)
	// create the default device, exit if it fails
	id <MTLDevice> mtl_device = MTLCreateSystemDefaultDevice();
	if(!mtl_device) return;
	NSArray <id<MTLDevice>>* mtl_devices = (NSArray <id<MTLDevice>>*)[NSArray arrayWithObjects:mtl_device, nil];
#else
	NSArray <id<MTLDevice>>* mtl_devices = MTLCopyAllDevices();
#endif
	if(!mtl_devices) return;
	
	// go through all found devices (for ios, this should be one)
	uint32_t device_num = 0;
	for(id <MTLDevice> dev in mtl_devices) {
		// check whitelist
		if(!whitelist.empty()) {
			const auto lc_dev_name = core::str_to_lower([[dev name] UTF8String]);
			bool found = false;
			for(const auto& entry : whitelist) {
				if(lc_dev_name.find(entry) != string::npos) {
					found = true;
					break;
				}
			}
			if(!found) continue;
		}
		
		// add device
		devices.emplace_back(make_unique<metal_device>());
		auto& device = (metal_device&)*devices.back();
		device.device = dev;
		device.context = this;
		device.name = [[dev name] UTF8String];
		device.type = (compute_device::TYPE)(uint32_t(compute_device::TYPE::GPU0) + device_num);
		++device_num;
		
		// TODO: eval MTLGPUFamily and MTLSoftwareVersion with macOS 10.15 and iOS 13.0
		
		// query device info that is a bit more complicated to get (not via direct device query)
		const auto device_info = metal_device_query::query(dev);
		if (device_info) {
			device.simd_width = device_info->simd_width;
			device.simd_range = { device.simd_width, device.simd_width };
		}
		
#if defined(FLOOR_IOS)
		// on ios, most of the device properties can't be querried, but are statically known (-> doc)
		device.vendor_name = "Apple";
		device.vendor = COMPUTE_VENDOR::APPLE;
		device.clock = 450; // actually unknown, and won't matter for now
		device.global_mem_size = (uint64_t)darwin_helper::get_memory_size();
		device.constant_mem_size = 65536; // no idea if this is correct, but it's the min required size for opencl 1.2
		
		// hard to make this forward compatible, there is no direct "get family" call
		// -> just try the first 17 types, good enough for now
		device.family_type = metal_device::FAMILY_TYPE::APPLE;
		uint32_t feature_set = 0;
		for(uint32_t i = 17; i > 0; --i) {
			if([dev supportsFeatureSet:(MTLFeatureSet)(i - 1)]) {
				feature_set = i - 1;
				break;
			}
		}
		
		// figure out which metal version we can use
		if (darwin_helper::get_system_version() >= 150000) {
			device.metal_software_version = METAL_VERSION::METAL_2_4;
			device.metal_language_version = METAL_VERSION::METAL_2_4;
		} else if (darwin_helper::get_system_version() >= 140000) {
			device.metal_software_version = METAL_VERSION::METAL_2_3;
			device.metal_language_version = METAL_VERSION::METAL_2_3;
		} else if (darwin_helper::get_system_version() >= 130000) {
			device.metal_software_version = METAL_VERSION::METAL_2_2;
			device.metal_language_version = METAL_VERSION::METAL_2_2;
		} else if (darwin_helper::get_system_version() >= 120000) {
			device.metal_software_version = METAL_VERSION::METAL_2_1;
			device.metal_language_version = METAL_VERSION::METAL_2_1;
		}
		
		// TODO: switch over to new MTLGPUFamily
		switch (feature_set) {
			default:
			case 0: // MTLFeatureSet_iOS_GPUFamily1_v1
			case 2: // MTLFeatureSet_iOS_GPUFamily1_v2
			case 5: // MTLFeatureSet_iOS_GPUFamily1_v3
			case 8: // MTLFeatureSet_iOS_GPUFamily1_v4
			case 12: // MTLFeatureSet_iOS_GPUFamily1_v5
				device.family_tier = 1;
				break;
			case 1: // MTLFeatureSet_iOS_GPUFamily2_v1
			case 3: // MTLFeatureSet_iOS_GPUFamily2_v2
			case 6: // MTLFeatureSet_iOS_GPUFamily2_v3
			case 9: // MTLFeatureSet_iOS_GPUFamily2_v4
			case 13: // MTLFeatureSet_iOS_GPUFamily2_v5
				device.family_tier = 2;
				break;
			case 4: // MTLFeatureSet_iOS_GPUFamily3_v1
			case 7: // MTLFeatureSet_iOS_GPUFamily3_v2
			case 10: // MTLFeatureSet_iOS_GPUFamily3_v3
			case 14: // MTLFeatureSet_iOS_GPUFamily3_v4
				device.family_tier = 3;
				break;
			case 11: // MTLFeatureSet_iOS_GPUFamily4_v1
			case 15: // MTLFeatureSet_iOS_GPUFamily4_v2
				device.family_tier = 4;
				break;
			case 16: // MTLFeatureSet_iOS_GPUFamily5_v1
				device.family_tier = 5;
				break;
		}
		
		// init statically known device information (pulled from AGXMetal/AGXG*Device and apples doc)
		switch (device.family_tier) {
			// A7/A7X
			default:
			case 1:
				device.units = 4; // G6430
				device.mem_clock = 1600; // ram clock
				device.max_image_1d_dim = { 8192 };
				device.max_image_2d_dim = { 8192, 8192 };
				device.max_total_local_size = 512;
				break;
			
			// A8/A8X
			case 2:
				if(device.name.find("A8X") != string::npos) {
					device.units = 8; // GXA6850
				}
				else {
					device.units = 4; // GX6450
				}
				device.mem_clock = 1600; // ram clock
				device.max_image_1d_dim = { 8192 };
				device.max_image_2d_dim = { 8192, 8192 };
				device.max_total_local_size = 512;
				break;
			
			// A9/A9X and A10/A10X
			case 3:
				if(device.name.find("A9X") != string::npos ||
				   device.name.find("A10X") != string::npos) {
					device.units = 12; // GT7800/7900?
				}
				else {
					device.units = 6; // GT7600 / GT7600 Plus
				}
				device.mem_clock = 1600; // TODO: ram clock
				device.max_image_1d_dim = { 16384 };
				device.max_image_2d_dim = { 16384, 16384 };
				device.max_total_local_size = 512;
				break;
			
			// A11 and A12
			case 4:
				device.units = 3; // Apple custom
				device.mem_clock = 1600; // TODO: ram clock
				device.max_image_1d_dim = { 16384 };
				device.max_image_2d_dim = { 16384, 16384 };
				device.max_total_local_size = 1024;
				break;
			
			// A12/A12X/A12Z
			case 5:
				device.units = 4; // Apple custom
				device.mem_clock = 1600; // TODO: ram clock
				device.max_image_1d_dim = { 16384 };
				device.max_image_2d_dim = { 16384, 16384 };
				device.max_total_local_size = 1024;
				break;
				
			// A13
			case 6:
				device.units = 4; // Apple custom (TODO: correct number)
				device.mem_clock = 1600; // TODO: ram clock
				device.max_image_1d_dim = { 16384 };
				device.max_image_2d_dim = { 16384, 16384 };
				device.max_total_local_size = 1024;
				break;
				
			// A14 / M1
			case 7:
				device.units = 8;
				device.mem_clock = 1600; // TODO: ram clock
				device.max_image_1d_dim = { 16384 };
				device.max_image_2d_dim = { 16384, 16384 };
				device.max_total_local_size = 1024;
				break;
				
			// A15
			case 8:
				device.units = 8;
				device.mem_clock = 1600; // TODO: ram clock
				device.max_image_1d_dim = { 16384 };
				device.max_image_2d_dim = { 16384, 16384 };
				device.max_total_local_size = 1024;
				break;
		}
		device.local_mem_size = 16384; // fallback
		device.local_mem_size = [dev maxThreadgroupMemoryLength];
		device.max_global_size = { 0xFFFFFFFFu };
		device.double_support = false; // double config is 0
		device.unified_memory = true; // always true
		device.max_image_1d_buffer_dim = { 0 }; // N/A on metal
		device.max_image_3d_dim = { 2048, 2048, 2048 };
		if (device.simd_width == 0) {
			device.simd_width = 32; // always 32 for powervr 6/7 series and apple A* series
			device.simd_range = { device.simd_width, device.simd_width };
		}
		device.image_cube_write_support = false;
		
		// check for indirect command support
		// NOTE: while initially supported in iOS 12.0, we do require iOS 13.0 functionality
		if (@available(iOS 13.0, *)) {
			if ([dev supportsFeatureSet:MTLFeatureSet_iOS_GPUFamily3_v4]) {
				device.indirect_command_support = true;
				device.indirect_render_command_support = true;
				device.indirect_compute_command_support = true;
			}
		}
		
		// tessellation is supported since A9
		if (device.family_tier >= 3) {
			device.tessellation_support = true;
			// 64 since A12, 16 before that
			device.max_tessellation_factor = (device.family_tier >= 5 ? 64u : 16u);
		}
#else
		__unsafe_unretained id <MTLDeviceSPI> dev_spi = (id <MTLDeviceSPI>)dev;
		
		// on os x, we can get to the device properties through MTLDeviceSPI
		device.vendor_name = [[dev_spi vendorName] UTF8String];
		const auto lc_vendor_name = core::str_to_lower(device.vendor_name);
		if (lc_vendor_name.find("nvidia") != string::npos) {
			device.vendor = COMPUTE_VENDOR::NVIDIA;
			if (device.simd_width == 0) {
				device.simd_width = 32;
				device.simd_range = { device.simd_width, device.simd_width };
			}
		} else if (lc_vendor_name.find("intel") != string::npos) {
			device.vendor = COMPUTE_VENDOR::INTEL;
			if (device.simd_width == 0) {
				device.simd_width = 16; // variable (8, 16 or 32), but 16 is a good estimate
				device.simd_range = { 8, 32 };
			}
		} else if (lc_vendor_name.find("amd") != string::npos) {
			device.vendor = COMPUTE_VENDOR::AMD;
		} else if (lc_vendor_name.find("apple") != string::npos) {
			device.vendor = COMPUTE_VENDOR::APPLE;
		} else {
			device.vendor = COMPUTE_VENDOR::UNKNOWN;
		}
		device.global_mem_size = 1024ull * 1024ull * 1024ull; // assume 1GiB for now
		if ([dev_spi respondsToSelector:@selector(dedicatedMemorySize)]) {
			device.global_mem_size = [dev_spi dedicatedMemorySize];
		}
		device.constant_mem_size = 65536; // can't query this, so assume opencl minimum
		if ([dev_spi respondsToSelector:@selector(hasUnifiedMemory)]) {
			device.unified_memory = [dev_spi hasUnifiedMemory];
		}
		
		// there is no direct way of querying the highest available feature set
		// -> find the highest (currently known) version
		device.family_type = metal_device::FAMILY_TYPE::MAC;
		uint32_t feature_set = 10000;
		for (uint32_t fs_version = 10005; fs_version >= 10000; --fs_version) {
			if ([dev supportsFeatureSet:(MTLFeatureSet)(fs_version)]) {
				feature_set = fs_version;
				break;
			}
		}
		
		switch (feature_set) {
			default:
			case 10000: // MTLFeatureSet_macOS_GPUFamily1_v1
			case 10001: // MTLFeatureSet_macOS_GPUFamily1_v2
			case 10002: // MTLFeatureSet_macOS_ReadWriteTextureTier2
			case 10003: // MTLFeatureSet_macOS_GPUFamily1_v3
			case 10004: // MTLFeatureSet_macOS_GPUFamily1_v4
				device.family_tier = 1;
				break;
			case 10005: // MTLFeatureSet_macOS_GPUFamily2_v1
				device.family_tier = 2;
				break;
		}
		
		if ([dev supportsFeatureSet:(MTLFeatureSet)10002]) {
			// NOTE: MTLFeatureSet_macOS_ReadWriteTextureTier2 is also v2, but with h/w image r/w support
			//device.image_read_write_support = true; // TODO: enable this when supported by the compiler
		}

		device.local_mem_size = [dev_spi maxComputeThreadgroupMemory];
		device.max_total_local_size = (uint32_t)[dev_spi maxTotalComputeThreadsPerThreadgroup];
		// we sadly can't query the amount of compute units directly
		// -> we can figure out the amount for AMD GPUs via ioreg (-> metal_device_query)
		device.units = (device_info ? device_info->units : 0u);
		device.clock = 0;
		device.mem_clock = 0;
		device.max_global_size = { 0xFFFFFFFFu };
		device.double_support = ([dev_spi doubleFPConfig] > 0);
		device.max_image_1d_buffer_dim = { 0 }; // N/A on metal
		device.max_image_1d_dim = { (uint32_t)[dev_spi maxTextureWidth1D] };
		device.max_image_2d_dim = { (uint32_t)[dev_spi maxTextureWidth2D], (uint32_t)[dev_spi maxTextureHeight2D] };
		device.max_image_3d_dim = { (uint32_t)[dev_spi maxTextureWidth3D], (uint32_t)[dev_spi maxTextureHeight3D], (uint32_t)[dev_spi maxTextureDepth3D] };
		device.image_cube_write_support = true;
		device.image_cube_array_support = true;
		device.image_cube_array_write_support = true;
		
		// figure out which metal version we can use
		if (darwin_helper::get_system_version() >= 120000) {
			device.metal_software_version = METAL_VERSION::METAL_2_4;
			device.metal_language_version = METAL_VERSION::METAL_2_4;
		} else if (darwin_helper::get_system_version() >= 110000) {
			device.metal_software_version = METAL_VERSION::METAL_2_3;
			device.metal_language_version = METAL_VERSION::METAL_2_3;
		} else if (darwin_helper::get_system_version() >= 101500) {
			device.metal_software_version = METAL_VERSION::METAL_2_2;
			device.metal_language_version = METAL_VERSION::METAL_2_2;
		} else if (darwin_helper::get_system_version() >= 101400) {
			device.metal_software_version = METAL_VERSION::METAL_2_1;
			device.metal_language_version = METAL_VERSION::METAL_2_1;
		} else if (darwin_helper::get_system_version() >= 101300) {
			device.metal_software_version = METAL_VERSION::METAL_2_0;
			device.metal_language_version = METAL_VERSION::METAL_2_0;
		}
		
		// Metal 2.0+ on macOS supports sub-groups and shuffle
		device.sub_group_support = true;
		device.sub_group_shuffle_support = true;
		
		// check for indirect command support
		// NOTE: while initially supported in macOS 10.14, we do require macOS 11.0 functionality
		if (@available(macOS 11.0, *)) {
			if ([dev supportsFeatureSet:MTLFeatureSet_macOS_GPUFamily2_v1]) {
				device.indirect_command_support = true;
				device.indirect_render_command_support = true;
				device.indirect_compute_command_support = true;
			}
		}
		
		// tessellation is always supported on macOS with 64 max factor
		device.tessellation_support = true;
		device.max_tessellation_factor = 64u;
#endif
		device.max_mem_alloc = 1024ull * 1024ull * 1024ull; // fixed 1GiB since 10.12
		if ([dev respondsToSelector:@selector(maxBufferLength)]) {
			device.max_mem_alloc = [dev maxBufferLength]; // iOS 12.0+ / macOS 10.14+
		}
		// adjust global memory size (might have been invalid)
		device.global_mem_size = max(device.global_mem_size, device.max_mem_alloc);
		device.max_group_size = { 0xFFFFFFFFu };
		device.max_local_size = {
			(uint32_t)[dev maxThreadsPerThreadgroup].width,
			(uint32_t)[dev maxThreadsPerThreadgroup].height,
			(uint32_t)[dev maxThreadsPerThreadgroup].depth
		};
		log_msg("max total local size: $'", device.max_total_local_size);
		log_msg("max local size: $'", device.max_local_size);
		log_msg("max global size: $'", device.max_global_size);
		log_msg("SIMD width: $", device.simd_width);
		log_msg("argument buffer tier: $", [dev argumentBuffersSupport] + 1);
		log_msg("indirect commands: $", device.indirect_command_support ? "yes" : "no");
		
		device.max_mip_levels = image_mip_level_count_from_max_dim(std::max(std::max(device.max_image_2d_dim.max_element(),
																					 device.max_image_3d_dim.max_element()),
																			device.max_image_1d_dim));
		
		if ([dev respondsToSelector:@selector(supportsShaderBarycentricCoordinates)]) {
			device.barycentric_coord_support = [dev supportsShaderBarycentricCoordinates];
			device.primitive_id_support = device.barycentric_coord_support;
		}
		
		// done
		supported = true;
		platform_vendor = COMPUTE_VENDOR::APPLE;
		log_debug("GPU (global: $' MB, local: $' bytes, unified: $): $, Metal $, family type $ tier $",
				  (uint32_t)(device.global_mem_size / 1024ull / 1024ull),
				  device.local_mem_size,
				  (device.unified_memory ? "yes" : "no"),
				  device.name,
				  metal_version_to_string(device.metal_software_version),
				  metal_device::family_type_to_string(device.family_type),
				  device.family_tier);
	}
	
	// check if there is any supported / whitelisted device
	if(devices.empty()) {
		log_error("no valid metal device found!");
		return;
	}
	
	// figure out the fastest device
#if defined(FLOOR_IOS)
	// only one device on ios
	fastest_gpu_device = devices[0].get();
	fastest_device = fastest_gpu_device;
#else
	// on os x, this is tricky, because we don't get the compute units count and clock speed
	// -> assume devices are returned in order of their speed
	// -> assume that nvidia and amd cards are faster than intel cards
	fastest_gpu_device = devices[0].get(); // start off with the first device
	if(fastest_gpu_device->vendor != COMPUTE_VENDOR::NVIDIA &&
	   fastest_gpu_device->vendor != COMPUTE_VENDOR::AMD) { // if this is false, we already have a nvidia/amd device
		for(size_t i = 1; i < devices.size(); ++i) {
			if(devices[i]->vendor == COMPUTE_VENDOR::NVIDIA ||
			   devices[i]->vendor == COMPUTE_VENDOR::AMD) {
				// found a nvidia or amd device, consider it the fastest
				fastest_gpu_device = devices[i].get();
				break;
			}
		}
	}
	fastest_device = fastest_gpu_device;
#endif
	log_debug("fastest GPU device: $", fastest_gpu_device->name);
	
	// create an internal queue and null buffer for each device
	for(auto& dev : devices) {
		// queue
		auto dev_queue = create_queue(*dev);
		internal_queues.insert_or_assign(*dev, dev_queue);
		((metal_device&)*dev).internal_queue = dev_queue.get();
		
		// null buffer
		auto null_buffer = create_buffer(*dev_queue, aligned_ptr<int>::page_size, COMPUTE_MEMORY_FLAG::READ | COMPUTE_MEMORY_FLAG::HOST_READ_WRITE);
		null_buffer->zero(*dev_queue);
		internal_null_buffers.insert_or_assign(*dev, null_buffer);
	}
	
	// init renderer
	if (enable_renderer) {
		render_device = (const metal_device*)fastest_gpu_device;
		auto mtl_dev = render_device->device;
		view = darwin_helper::create_metal_view(floor::get_window(), mtl_dev, hdr_metadata);
		if (view == nullptr) {
			log_error("failed to create Metal view!");
			supported = false;
		}
		
#if !defined(FLOOR_NO_VR)
		if (vr_ctx) {
			if (!init_vr_renderer()) {
				log_error("failed to init VR renderer");
				vr_ctx = nullptr;
			}
		}
#endif
	}
}

shared_ptr<compute_queue> metal_compute::create_queue(const compute_device& dev) const {
	id <MTLCommandQueue> queue = [((const metal_device&)dev).device newCommandQueue];
	if(queue == nullptr) {
		log_error("failed to create command queue");
		return {};
	}
	
	auto ret = make_shared<metal_queue>(dev, queue);
	queues.push_back(ret);
	return ret;
}

const compute_queue* metal_compute::get_device_default_queue(const compute_device& dev) const {
	if (const auto iter = internal_queues.find(dev); iter != internal_queues.end()) {
		return iter->second.get();
	}
	log_error("no default queue exists for this device: $!", dev.name);
	return nullptr;
}

const metal_buffer* metal_compute::get_null_buffer(const compute_device& dev) const {
	if (const auto iter = internal_null_buffers.find(dev); iter != internal_null_buffers.end()) {
		return (const metal_buffer*)iter->second.get();
	}
	log_error("no null-buffer exists for this device: $!", dev.name);
	return nullptr;
}

shared_ptr<compute_buffer> metal_compute::create_buffer(const compute_queue& cqueue,
														const size_t& size, const COMPUTE_MEMORY_FLAG flags,
														const uint32_t opengl_type) const {
	return make_shared<metal_buffer>(cqueue, size, flags, opengl_type);
}

shared_ptr<compute_buffer> metal_compute::create_buffer(const compute_queue& cqueue,
														const size_t& size, void* data,
														const COMPUTE_MEMORY_FLAG flags,
														const uint32_t opengl_type) const {
	return make_shared<metal_buffer>(cqueue, size, data, flags, opengl_type);
}

shared_ptr<compute_buffer> metal_compute::wrap_buffer(const compute_queue& cqueue floor_unused,
													  const uint32_t opengl_buffer floor_unused,
													  const uint32_t opengl_type floor_unused,
													  const COMPUTE_MEMORY_FLAG flags floor_unused) const {
	log_error("opengl buffer sharing not supported by metal!");
	return {};
}

shared_ptr<compute_buffer> metal_compute::wrap_buffer(const compute_queue& cqueue floor_unused,
													  const uint32_t opengl_buffer floor_unused,
													  const uint32_t opengl_type floor_unused,
													  void* data floor_unused,
													  const COMPUTE_MEMORY_FLAG flags floor_unused) const {
	log_error("opengl buffer sharing not supported by metal!");
	return {};
}

shared_ptr<compute_image> metal_compute::create_image(const compute_queue& cqueue,
													  const uint4 image_dim,
													  const COMPUTE_IMAGE_TYPE image_type,
													  const COMPUTE_MEMORY_FLAG flags,
													  const uint32_t opengl_type) const {
	return make_shared<metal_image>(cqueue, image_dim, image_type, nullptr, flags, opengl_type);
}

shared_ptr<compute_image> metal_compute::create_image(const compute_queue& cqueue,
													  const uint4 image_dim,
													  const COMPUTE_IMAGE_TYPE image_type,
													  void* data,
													  const COMPUTE_MEMORY_FLAG flags,
													  const uint32_t opengl_type) const {
	return make_shared<metal_image>(cqueue, image_dim, image_type, data, flags, opengl_type);
}

shared_ptr<compute_image> metal_compute::wrap_image(const compute_queue& cqueue floor_unused,
													const uint32_t opengl_image floor_unused,
													const uint32_t opengl_target floor_unused,
													const COMPUTE_MEMORY_FLAG flags floor_unused) const {
	log_error("opengl image sharing not supported by metal!");
	return {};
}

shared_ptr<compute_image> metal_compute::wrap_image(const compute_queue& cqueue floor_unused,
													const uint32_t opengl_image floor_unused,
													const uint32_t opengl_target floor_unused,
													void* data floor_unused,
													const COMPUTE_MEMORY_FLAG flags floor_unused) const {
	log_error("opengl image sharing not supported by metal!");
	return {};
}

static shared_ptr<metal_program> add_metal_program(metal_program::program_map_type&& prog_map,
												   vector<shared_ptr<metal_program>>* programs,
												   atomic_spin_lock& programs_lock) REQUIRES(!programs_lock) {
	// create the program object, which in turn will create kernel objects for all kernel functions in the program,
	// for all devices contained in the program map
	auto prog = make_shared<metal_program>(move(prog_map));
	{
		GUARD(programs_lock);
		programs->push_back(prog);
	}
	return prog;
}

shared_ptr<compute_program> metal_compute::add_universal_binary(const string& file_name) {
	auto bins = universal_binary::load_dev_binaries_from_archive(file_name, *this);
	if (bins.ar == nullptr || bins.dev_binaries.empty()) {
		log_error("failed to load universal binary: $", file_name);
		return {};
	}
	
	// move the archive memory to a shared_ptr
	// NOTE: we need to do this because dispatch_data_t/newLibraryWithData will access the data after leaving this function,
	//       when the archive will have been destructed already if kept in the local unique_ptr
	shared_ptr<universal_binary::archive> ar(bins.ar.release());
	
	// create the program
	metal_program::program_map_type prog_map;
	prog_map.reserve(devices.size());
	for (size_t i = 0, dev_count = devices.size(); i < dev_count; ++i) {
		const auto& mtl_dev = (const metal_device&)*devices[i];
		const auto& dev_best_bin = bins.dev_binaries[i];
		const auto func_info = universal_binary::translate_function_info(dev_best_bin.first->functions);
		
		metal_program::metal_program_entry entry;
		entry.archive = ar; // ensure we keep the archive memory
		entry.functions = func_info;
		
		NSError* err { nil };
		dispatch_data_t lib_data = dispatch_data_create(dev_best_bin.first->data.data(), dev_best_bin.first->data.size(),
														dispatch_get_main_queue(), ^{} /* must be non-default */);
		entry.program = [mtl_dev.device newLibraryWithData:lib_data error:&err];
		if (!entry.program) {
			log_error("failed to create metal program/library for device $: $",
					  mtl_dev.name, (err != nil ? [[err localizedDescription] UTF8String] : "unknown error"));
			return {};
		}
		entry.valid = true;
		
		prog_map.insert_or_assign(mtl_dev, entry);
	}
	
	return add_metal_program(move(prog_map), &programs, programs_lock);
}

static metal_program::metal_program_entry create_metal_program(const metal_device& device floor_unused_on_ios,
															   llvm_toolchain::program_data program) {
	metal_program::metal_program_entry ret;
	ret.functions = program.functions;
	
	if(!program.valid) {
		return ret;
	}
	
#if !defined(FLOOR_IOS) // can only do this on os x
	// create the program/library object and build it (note: also need to create an dispatcht_data_t object ...)
	NSError* err { nil };
	const auto lib_file_name = [NSString stringWithUTF8String:program.data_or_filename.c_str()];
	ret.program = [device.device newLibraryWithFile:lib_file_name
											  error:&err];
	if(!floor::get_toolchain_keep_temp()) {
		// cleanup
		if(!floor::get_toolchain_debug()) {
			core::system("rm "s + program.data_or_filename);
		}
	}
	if(!ret.program) {
		log_error("failed to create metal program/library for device $: $",
				  device.name, (err != nil ? [[err localizedDescription] UTF8String] : "unknown error"));
		return ret;
	}
	
	// TODO: print out the build log
	ret.valid = true;
	return ret;
#else
	log_error("this is not supported on iOS!");
	return ret;
#endif
}

shared_ptr<compute_program> metal_compute::add_program_file(const string& file_name,
															const string additional_options) {
	return add_program_file(file_name, compile_options { .cli = additional_options });
}

shared_ptr<compute_program> metal_compute::add_program_file(const string& file_name,
															compile_options options) {
	// compile the source file for all devices in the context
	metal_program::program_map_type prog_map;
	prog_map.reserve(devices.size());
	options.target = llvm_toolchain::TARGET::AIR;
	for(const auto& dev : devices) {
		prog_map.insert_or_assign((const metal_device&)*dev,
								  create_metal_program((const metal_device&)*dev,
													   llvm_toolchain::compile_program_file(*dev, file_name, options)));
	}
	return add_metal_program(move(prog_map), &programs, programs_lock);
}

shared_ptr<compute_program> metal_compute::add_program_source(const string& source_code,
															  const string additional_options) {
	return add_program_source(source_code, compile_options { .cli = additional_options });
}

shared_ptr<compute_program> metal_compute::add_program_source(const string& source_code,
															  compile_options options) {
	// compile the source code for all devices in the context
	metal_program::program_map_type prog_map;
	prog_map.reserve(devices.size());
	options.target = llvm_toolchain::TARGET::AIR;
	for(const auto& dev : devices) {
		prog_map.insert_or_assign((const metal_device&)*dev,
								  create_metal_program((const metal_device&)*dev,
													   llvm_toolchain::compile_program(*dev, source_code, options)));
	}
	return add_metal_program(move(prog_map), &programs, programs_lock);
}

shared_ptr<compute_program> metal_compute::add_precompiled_program_file(const string& file_name,
																		const vector<llvm_toolchain::function_info>& functions) {
	log_debug("loading mtllib: $", file_name);
	
	// assume pre-compiled program is the same for all devices
	metal_program::program_map_type prog_map;
	prog_map.reserve(devices.size());
	for(const auto& dev : devices) {
		metal_program::metal_program_entry entry;
		entry.functions = functions;
		
		NSError* err { nil };
		const auto lib_file_name = [NSString stringWithUTF8String:file_name.c_str()];
		entry.program = [((const metal_device&)*dev).device newLibraryWithFile:lib_file_name
																		 error:&err];
		if(!entry.program) {
			log_error("failed to create metal program/library for device $: $",
					  dev->name, (err != nil ? [[err localizedDescription] UTF8String] : "unknown error"));
			continue;
		}
		entry.valid = true;
		
		prog_map.insert_or_assign((const metal_device&)*dev, entry);
	}
	return add_metal_program(move(prog_map), &programs, programs_lock);
}

shared_ptr<compute_program::program_entry> metal_compute::create_program_entry(const compute_device& device,
																			   llvm_toolchain::program_data program,
																			   const llvm_toolchain::TARGET) {
	return make_shared<metal_program::metal_program_entry>(create_metal_program((const metal_device&)device, program));
}

shared_ptr<compute_program> metal_compute::create_metal_test_program(shared_ptr<compute_program::program_entry> entry) {
	const auto metal_entry = (const metal_program::metal_program_entry*)entry.get();
	
	// find the device the specified program has been compiled for
	const metal_device* metal_dev = nullptr;
	for(auto& dev : devices) {
		if(((const metal_device&)*dev).device == [metal_entry->program device]) {
			metal_dev = (const metal_device*)&dev;
			break;
		}
	}
	if(metal_dev == nullptr) {
		log_error("program device is not part of this context");
		return nullptr;
	}
	
	// create/return the program
	metal_program::program_map_type prog_map;
	prog_map.insert(*metal_dev, *metal_entry);
	return make_shared<metal_program>(move(prog_map));
}

unique_ptr<indirect_command_pipeline> metal_compute::create_indirect_command_pipeline(const indirect_command_description& desc) const {
	auto pipeline = make_unique<metal_indirect_command_pipeline>(desc, devices);
	if (!pipeline || !pipeline->is_valid()) {
		return {};
	}
	return pipeline;
}

MTLPixelFormat metal_compute::get_metal_renderer_pixel_format() const {
	if (view == nullptr) {
		return MTLPixelFormatInvalid;
	}
	return darwin_helper::get_metal_pixel_format(view);
}

id <CAMetalDrawable> metal_compute::get_metal_next_drawable(id <MTLCommandBuffer> cmd_buffer) const {
	if (view == nullptr) {
		return nil;
	}
	return darwin_helper::get_metal_next_drawable(view, cmd_buffer);
}

bool metal_compute::init_vr_renderer() {
#if !defined(FLOOR_NO_VR)
	if (!vr_ctx) {
		return false;
	}
	
	const auto& dev_queue = *get_device_default_queue(*render_device);
	
	const uint4 vr_screen_dim { floor::get_vr_physical_screen_size(), 2, 0 };
	const COMPUTE_IMAGE_TYPE vr_image_type = (COMPUTE_IMAGE_TYPE::RGBA8UI_NORM |
											  COMPUTE_IMAGE_TYPE::IMAGE_2D_ARRAY |
											  COMPUTE_IMAGE_TYPE::FLAG_RENDER_TARGET |
											  COMPUTE_IMAGE_TYPE::READ_WRITE);
	for (uint32_t i = 0; i < vr_image_count; ++i) {
		vr_images[i].image = create_image(dev_queue, vr_screen_dim, vr_image_type,
										  COMPUTE_MEMORY_FLAG::READ_WRITE | COMPUTE_MEMORY_FLAG::HOST_READ_WRITE);
		vr_images[i].image->set_debug_label("VR screen image #" + to_string(i));
	}
	
	return true;
#else
	return false;
#endif
}

shared_ptr<compute_image> metal_compute::get_metal_next_vr_drawable() const NO_THREAD_SAFETY_ANALYSIS {
	if (!vr_ctx) {
		return {};
	}
	
	// manual index advance
	vr_image_index = (vr_image_index + 1) % vr_image_count;
	
	// lock this image until it has been submitted for present (also blocks until the wanted image is available)
	vr_images[vr_image_index].image_lock.lock();
	return vr_images[vr_image_index].image;
}

#if !defined(FLOOR_NO_VR)
void metal_compute::present_metal_vr_drawable(const compute_queue& cqueue floor_unused_on_ios,
											  const compute_image& img floor_unused_on_ios) const NO_THREAD_SAFETY_ANALYSIS {
	if (!vr_ctx) {
		return;
	}
	vr_ctx->present(cqueue, img);
	vr_ctx->update();
	
	// unlock image again
	for (auto& vr_img : vr_images) {
		if (vr_img.image.get() == &img) {
			vr_img.image_lock.unlock();
			break;
		}
	}
}
#else
void metal_compute::present_metal_vr_drawable(const compute_queue&, const compute_image&) const {
	// nop
}
#endif

unique_ptr<graphics_pipeline> metal_compute::create_graphics_pipeline(const render_pipeline_description& pipeline_desc,
																	  const bool with_multi_view_support) const {
	auto pipeline = make_unique<metal_pipeline>(pipeline_desc, devices, with_multi_view_support && (vr_ctx != nullptr));
	if (!pipeline || !pipeline->is_valid()) {
		return {};
	}
	return pipeline;
}

unique_ptr<graphics_pass> metal_compute::create_graphics_pass(const render_pass_description& pass_desc,
															  const bool with_multi_view_support) const {
	auto pass = make_unique<metal_pass>(pass_desc, with_multi_view_support && (vr_ctx != nullptr));
	if (!pass || !pass->is_valid()) {
		return {};
	}
	return pass;
}

unique_ptr<graphics_renderer> metal_compute::create_graphics_renderer(const compute_queue& cqueue,
																	  const graphics_pass& pass,
																	  const graphics_pipeline& pipeline,
																	  const bool create_multi_view_renderer) const {
	if (create_multi_view_renderer && !is_vr_supported()) {
		log_error("can't create a multi-view/VR graphics renderer when VR is not supported");
		return {};
	}
	
	auto renderer = make_unique<metal_renderer>(cqueue, pass, pipeline, create_multi_view_renderer);
	if (!renderer || !renderer->is_valid()) {
		return {};
	}
	return renderer;
}

COMPUTE_IMAGE_TYPE metal_compute::get_renderer_image_type() const {
	switch (get_metal_renderer_pixel_format()) {
		case MTLPixelFormatBGRA8Unorm:
			return COMPUTE_IMAGE_TYPE::BGRA8UI_NORM;
		case MTLPixelFormatBGRA8Unorm_sRGB:
			return COMPUTE_IMAGE_TYPE::BGRA8UI_NORM | COMPUTE_IMAGE_TYPE::FLAG_SRGB;
		case MTLPixelFormatBGR10A2Unorm:
			return COMPUTE_IMAGE_TYPE::A2BGR10UI_NORM;
		case MTLPixelFormatRGBA16Float:
			return COMPUTE_IMAGE_TYPE::RGBA16F;
#if defined(FLOOR_IOS)
		case MTLPixelFormatBGR10_XR:
			return COMPUTE_IMAGE_TYPE::BGR10UI_NORM;
		case MTLPixelFormatBGR10_XR_sRGB:
			return COMPUTE_IMAGE_TYPE::BGR10UI_NORM | COMPUTE_IMAGE_TYPE::FLAG_SRGB;
		case MTLPixelFormatBGRA10_XR:
			return COMPUTE_IMAGE_TYPE::BGRA10UI_NORM;
		case MTLPixelFormatBGRA10_XR_sRGB:
			return COMPUTE_IMAGE_TYPE::BGRA10UI_NORM | COMPUTE_IMAGE_TYPE::FLAG_SRGB;
#endif
		default: break;
	}
	return COMPUTE_IMAGE_TYPE::NONE;
}

uint4 metal_compute::get_renderer_image_dim() const {
	if (!vr_ctx) {
		if (view) {
			return { darwin_helper::get_metal_view_dim(view), 0, 0 };
		}
		return {};
	} else {
		return vr_images[0].image->get_image_dim();
	}
}

bool metal_compute::is_vr_supported() const {
	return (vr_ctx ? true : false);
}

vr_context* metal_compute::get_renderer_vr_context() const {
	return vr_ctx;
}

void metal_compute::set_hdr_metadata(const hdr_metadata_t& hdr_metadata_) {
	compute_context::set_hdr_metadata(hdr_metadata_);
	if (view != nullptr) {
		darwin_helper::set_metal_view_hdr_metadata(view, hdr_metadata);
	}
}

float metal_compute::get_hdr_range_max() const {
	if (view != nullptr) {
		const auto edr_max = darwin_helper::get_metal_view_edr_max(view);
		if (edr_max <= 1.0f) {
			// SDR
			return 1.0f;
		}
		// normalize max nits to extended linear range (interpreting 1.0 as 80 nits):
		// (max nits * (100 nits / 80 nits)) / 100 nits
		return darwin_helper::get_metal_view_hdr_max_nits(view) * (1.0f / 80.0f);
	}
	return compute_context::get_hdr_range_max();
}

float metal_compute::get_hdr_display_max_nits() const {
	if (view != nullptr) {
		return darwin_helper::get_metal_view_hdr_max_nits(view);
	}
	return compute_context::get_hdr_display_max_nits();
}

bool metal_compute::start_metal_capture(const compute_device& dev, const string& file_name) const {
#if (defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED >= 101500) || \
	(defined(__IPHONE_OS_VERSION_MAX_ALLOWED) && __IPHONE_OS_VERSION_MAX_ALLOWED >= 130000)
	MTLCaptureManager* capture_manager = [MTLCaptureManager sharedCaptureManager];
	if (![capture_manager supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
		log_error("can't capture GPU trace to file");
		return false;
	}
	
	MTLCaptureDescriptor* capture_desc = [[MTLCaptureDescriptor alloc] init];
	capture_manager.defaultCaptureScope =
		[capture_manager newCaptureScopeWithDevice:((const metal_device&)dev).device];
	capture_desc.captureObject = capture_manager.defaultCaptureScope;
	auto file_name_nsstr = [NSString stringWithUTF8String:file_name.c_str()];
	capture_desc.outputURL = [NSURL fileURLWithPath:file_name_nsstr];
	capture_desc.destination = MTLCaptureDestinationGPUTraceDocument;
	
	NSError* err { nil };
	if (![capture_manager startCaptureWithDescriptor:capture_desc error:&err]) {
		log_error("failed to start GPU trace capture: $",
				   (err != nil ? [[err localizedDescription] UTF8String] : "unknown error"));
		return false;
	}
	
	[capture_manager.defaultCaptureScope beginScope];
	
	return true;
#else
	return false;
#endif
}

bool metal_compute::stop_metal_capture() const {
#if (defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED >= 101500) || \
	(defined(__IPHONE_OS_VERSION_MAX_ALLOWED) && __IPHONE_OS_VERSION_MAX_ALLOWED >= 130000)
	MTLCaptureManager* capture_manager = [MTLCaptureManager sharedCaptureManager];
	[capture_manager.defaultCaptureScope endScope];
	[capture_manager stopCapture];
	capture_manager.defaultCaptureScope = nil;
	return true;
#else
	return false;
#endif
}

#endif
