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

#include <floor/compute/vulkan/vulkan_image.hpp>

#if !defined(FLOOR_NO_VULKAN)

#include <floor/core/logger.hpp>
#include <floor/core/core.hpp>
#include <floor/compute/vulkan/vulkan_queue.hpp>
#include <floor/compute/vulkan/vulkan_device.hpp>
#include <floor/compute/vulkan/vulkan_compute.hpp>

#if defined(__WINDOWS__)
#include <floor/core/platform_windows.hpp>
#include <floor/core/essentials.hpp>
#include <vulkan/vulkan_win32.h>
#endif

// TODO: proper error (return) value handling everywhere

vulkan_image::vulkan_image(const compute_queue& cqueue,
						   const uint4 image_dim_,
						   const COMPUTE_IMAGE_TYPE image_type_,
						   void* host_ptr_,
						   const COMPUTE_MEMORY_FLAG flags_,
						   const uint32_t opengl_type_,
						   const uint32_t external_gl_object_,
						   const opengl_image_info* gl_image_info) :
compute_image(cqueue, image_dim_, image_type_, host_ptr_, flags_,
			  opengl_type_, external_gl_object_, gl_image_info),
vulkan_memory((const vulkan_device&)cqueue.get_device(), &image) {
	const bool is_render_target = has_flag<COMPUTE_IMAGE_TYPE::FLAG_RENDER_TARGET>(image_type);
	
	VkImageUsageFlags usage = 0;
	switch(flags & COMPUTE_MEMORY_FLAG::READ_WRITE) {
		case COMPUTE_MEMORY_FLAG::READ:
			usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
			break;
		case COMPUTE_MEMORY_FLAG::WRITE:
			usage |= VK_IMAGE_USAGE_STORAGE_BIT;
			break;
		case COMPUTE_MEMORY_FLAG::READ_WRITE:
			usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
			break;
		// all possible cases handled
		default: floor_unreachable();
	}
	
	if (is_render_target) {
		if (!has_flag<COMPUTE_IMAGE_TYPE::FLAG_DEPTH>(image_type)) {
			usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		} else {
			usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		}
		
		// if readable: allow use as an input attachment
		if (has_flag<COMPUTE_IMAGE_TYPE::READ>(image_type)) {
			usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
		}
	}
	
	// must be able to write to the image when mip-map generation is enabled
	if(generate_mip_maps) {
		usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	}
	
	// always need this for now
	usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	
	// actually create the image
	if(!create_internal(true, cqueue, usage)) {
		return; // can't do much else
	}
}

bool vulkan_image::create_internal(const bool copy_host_data, const compute_queue& cqueue, const VkImageUsageFlags& usage) {
	auto vulkan_dev = ((const vulkan_device&)dev).device;
	const auto dim_count = image_dim_count(image_type);
	const bool is_array = has_flag<COMPUTE_IMAGE_TYPE::FLAG_ARRAY>(image_type);
	const bool is_cube = has_flag<COMPUTE_IMAGE_TYPE::FLAG_CUBE>(image_type);
	//const bool is_msaa = has_flag<COMPUTE_IMAGE_TYPE::FLAG_MSAA>(image_type); // TODO: msaa support
	const bool is_depth = has_flag<COMPUTE_IMAGE_TYPE::FLAG_DEPTH>(image_type);
	//const bool is_compressed = image_compressed(image_type); // TODO: check incompatible usage
	const bool is_read_only = has_flag<COMPUTE_IMAGE_TYPE::READ>(image_type) && !has_flag<COMPUTE_IMAGE_TYPE::WRITE>(image_type);
	const bool is_render_target = has_flag<COMPUTE_IMAGE_TYPE::FLAG_RENDER_TARGET>(image_type);
	const bool is_aliasing = has_flag<COMPUTE_MEMORY_FLAG::VULKAN_ALIASING>(flags);
	
	// format conversion
	const auto vk_format_opt = vulkan_format_from_image_type(image_type);
	if (!vk_format_opt) {
		log_error("unsupported image format: %s (%X)", image_type_to_string(image_type), image_type);
		return false;
	}
	vk_format = *vk_format_opt;
	
	// set shim format info if necessary
	set_shim_type_info();
	
	// dim handling
	const VkImageType vk_image_type = (dim_count == 1 ? VK_IMAGE_TYPE_1D :
									   dim_count == 2 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D);
	const VkExtent3D extent {
		.width = image_dim.x,
		.height = dim_count >= 2 ? image_dim.y : 1,
		.depth = dim_count >= 3 ? image_dim.z : 1,
	};
	if(is_cube) {
		if(extent.width != extent.height) {
			log_error("cube map width and height must be equal");
			return false;
		}
	}
	
	// TODO: when using linear memory, can also use VK_IMAGE_LAYOUT_PREINITIALIZED here
	const VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageLayout final_layout = VK_IMAGE_LAYOUT_GENERAL;
	
	// TODO: handle render targets via additional image transfer?
	VkAccessFlags dst_access_flags = 0;
	if(is_render_target) {
		if(!is_depth) {
			final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			dst_access_flags = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		}
		else {
			final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			dst_access_flags = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		}
	}

	// TODO: might want VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT later on
	VkImageCreateFlags vk_create_flags = 0;
	if (is_cube) {
		vk_create_flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	}
	if (is_aliasing) {
		vk_create_flags |= VK_IMAGE_CREATE_ALIAS_BIT;
	}
	
	// create the image
	const auto is_sharing = has_flag<COMPUTE_MEMORY_FLAG::VULKAN_SHARING>(flags);
	VkExternalMemoryImageCreateInfo ext_create_info;
	if (is_sharing) {
		ext_create_info = {
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
			.pNext = nullptr,
#if defined(__WINDOWS__)
			.handleTypes = (core::is_windows_8_or_higher() ?
							VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT :
							VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT),
#else
			.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
#endif
		};
	}
	const VkImageCreateInfo image_create_info {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = (is_sharing ? &ext_create_info : nullptr),
		.flags = vk_create_flags,
		.imageType = vk_image_type,
		.format = vk_format,
		.extent = extent,
		.mipLevels = mip_level_count,
		.arrayLayers = layer_count,
		.samples = VK_SAMPLE_COUNT_1_BIT, // TODO: msaa support
		.tiling = VK_IMAGE_TILING_OPTIMAL, // TODO: might want linear as well later on?
		.usage = usage,
		// NOTE: for performance reasons, we always want exclusive sharing
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr,
		.initialLayout = initial_layout,
	};
	VK_CALL_RET(vkCreateImage(vulkan_dev, &image_create_info, nullptr, &image),
				"image creation failed", false)

	// aliased array: create images for each plane
	const auto is_aliased_array = (is_aliasing && is_array);
	if (is_aliased_array) {
		const auto layer_count = image_layer_count(image_dim, image_type);
		image_aliased_layers.resize(layer_count, nullptr);

		auto image_layer_create_info = image_create_info;
		image_layer_create_info.arrayLayers = 1;
		image_layer_create_info.extent.depth = 1;
		for (uint32_t layer = 0; layer < layer_count; ++layer) {
			VK_CALL_RET(vkCreateImage(vulkan_dev, &image_layer_create_info, nullptr, &image_aliased_layers[layer]),
						"image layer creation failed", false)
		}
	}
	
	// export memory alloc info (if sharing is enabled)
	VkExportMemoryAllocateInfo export_alloc_info;
#if defined(__WINDOWS__)
	VkExportMemoryWin32HandleInfoKHR export_mem_win32_info;
#endif
	if (is_sharing) {
#if defined(__WINDOWS__)
		// Windows 8+ needs more detailed sharing info
		if (core::is_windows_8_or_higher()) {
			export_mem_win32_info = {
				.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
				.pNext = nullptr,
				// NOTE: SECURITY_ATTRIBUTES are only required if we want a child process to inherit this handle
				//       -> we don't need this, so set it to nullptr
				.pAttributes = nullptr,
				.dwAccess = (DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE),
				.name = nullptr,
			};
		}
#endif
		
		export_alloc_info = {
			.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
#if defined(__WINDOWS__)
			.pNext = (core::is_windows_8_or_higher() ? &export_mem_win32_info : nullptr),
			.handleTypes = (core::is_windows_8_or_higher() ?
							VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT :
							VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT),
#else
			.pNext = nullptr,
			.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
#endif
		};
	}
	
	// allocate / back it up
	VkMemoryRequirements mem_req;
	vkGetImageMemoryRequirements(vulkan_dev, image, &mem_req);
	allocation_size = mem_req.size;
	
	const VkMemoryAllocateInfo alloc_info {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = (is_sharing ? &export_alloc_info : nullptr),
		.allocationSize = allocation_size,
		.memoryTypeIndex = find_memory_type_index(mem_req.memoryTypeBits, true /* prefer device memory */,
												  is_sharing /* sharing requires device memory */),
	};
	VK_CALL_RET(vkAllocateMemory(vulkan_dev, &alloc_info, nullptr, &mem), "image allocation failed", false)
	VK_CALL_RET(vkBindImageMemory(vulkan_dev, image, mem, 0), "image allocation binding failed", false)

	// aliased array: back each layer
	if (is_aliased_array) {
		VkMemoryRequirements layer_mem_req;
		vkGetImageMemoryRequirements(vulkan_dev, image_aliased_layers[0], &layer_mem_req);
		const auto per_layer_size = layer_mem_req.size;
		for (uint32_t layer = 0; layer < layer_count; ++layer) {
			VK_CALL_RET(vkBindImageMemory(vulkan_dev, image_aliased_layers[layer], mem, per_layer_size * layer),
						"image layer allocation binding failed", false)
		}
	}
	
	// create the view
	VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
	switch(dim_count) {
		case 1:
			view_type = (is_array ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D);
			break;
		case 2:
			if(!is_cube) {
				view_type = (is_array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
			}
			else {
				view_type = (is_array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE);
			}
			break;
		case 3:
			view_type = VK_IMAGE_VIEW_TYPE_3D;
			break;
		default: floor_unreachable();
	}
	
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	if(is_depth) {
		aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		if(has_flag<COMPUTE_IMAGE_TYPE::FLAG_STENCIL>(image_type)) {
			aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	}
	const VkImageSubresourceRange sub_rsrc_range {
		.aspectMask = aspect,
		.baseMipLevel = 0,
		.levelCount = mip_level_count,
		.baseArrayLayer = 0,
		.layerCount = layer_count,
	};
	
	const VkImageViewCreateInfo image_view_create_info {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.image = image,
		.viewType = view_type,
		.format = vk_format,
		.components = {
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY
		},
		.subresourceRange = sub_rsrc_range,
	};
	VK_CALL_RET(vkCreateImageView(vulkan_dev, &image_view_create_info, nullptr, &image_view),
				"image view creation failed", false)
	
	// transition to general layout or attachment layout (if render target)
	cur_access_mask = 0; // TODO: ?
	image_info.imageLayout = initial_layout;
	const auto transition_stage = (is_render_target ? VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT : VK_PIPELINE_STAGE_HOST_BIT);
	transition(cqueue, nullptr, dst_access_flags, final_layout, transition_stage, transition_stage);
	
	// update image desc info
	image_info.sampler = nullptr;
	image_info.imageView = image_view;
	image_info.imageLayout = final_layout; // TODO: need to keep track of this
	
	// if mip-mapping is enabled and the image is writable or mip-maps should be generated,
	// we need to create a per-level image view, so that kernels/shaders can actually write to each mip-map level
	// (Vulkan doesn't support this at this point, although SPIR-V does)
	if(is_mip_mapped && (generate_mip_maps || has_flag<COMPUTE_IMAGE_TYPE::WRITE>(image_type))) {
		mip_map_image_info.resize(device.max_mip_levels);
		mip_map_image_view.resize(device.max_mip_levels);
		const auto last_level = mip_level_count - 1;
		for(uint32_t i = 0; i < device.max_mip_levels; ++i) {
			mip_map_image_info[i].sampler = nullptr;
			
			// fill unused views with the last (1x1 level) view
			if(i > last_level) {
				mip_map_image_view[i] = mip_map_image_view[last_level];
				mip_map_image_info[i].imageView = mip_map_image_view[last_level];
				continue;
			}
			
			// create a view of a single mip level
			const VkImageSubresourceRange mip_sub_rsrc_range {
				.aspectMask = aspect,
				.baseMipLevel = i,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = layer_count,
			};
			
			const VkImageViewCreateInfo mip_image_view_create_info {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.pNext = nullptr,
				.flags = 0,
				.image = image,
				.viewType = view_type,
				.format = vk_format,
				.components = {
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY
				},
				.subresourceRange = mip_sub_rsrc_range,
			};
			VK_CALL_RET(vkCreateImageView(vulkan_dev, &mip_image_view_create_info, nullptr, &mip_map_image_view[i]),
						"mip-map image view creation failed", false)
			mip_map_image_info[i].imageView = mip_map_image_view[i];
		}
	}
	else {
		mip_map_image_info.resize(device.max_mip_levels, image_info);
		mip_map_image_view.resize(device.max_mip_levels, image_view);
	}
	update_mip_map_info();
	
	// buffer init from host data pointer
	if(copy_host_data &&
	   host_ptr != nullptr &&
	   !has_flag<COMPUTE_MEMORY_FLAG::NO_INITIAL_COPY>(flags)) {
		if(is_render_target) {
			log_error("can't initialize a render target with host data!");
		}
		else {
			if(!write_memory_data(cqueue, host_ptr,
								  (shim_image_type != image_type ? shim_image_data_size : image_data_size), 0,
								  (shim_image_type != image_type ? image_data_size : 0),
								  "failed to initialize image with host data (map failed)")) {
				return false;
			}
		}
	}
	
	// manually create mip-map chain
	if(generate_mip_maps) {
		generate_mip_map_chain(cqueue);
	}
	
	// transition image to its defined usage (render targets already have been transitioned)
	if (!is_render_target) {
		if (is_read_only) {
			transition_read(cqueue, nullptr);
		} else {
			transition_write(cqueue, nullptr);
		}
	}
	
	// get shared memory handle (if sharing is enabled)
	if (is_sharing) {
		const auto& vk_ctx = *((const vulkan_compute*)cqueue.get_device().context);
#if defined(__WINDOWS__)
		VkMemoryGetWin32HandleInfoKHR get_win32_handle {
			.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
			.pNext = nullptr,
			.memory = mem,
			.handleType = (VkExternalMemoryHandleTypeFlagBits)export_alloc_info.handleTypes,
		};
		VK_CALL_RET(vk_ctx.vulkan_get_memory_win32_handle(vulkan_dev, &get_win32_handle, &shared_handle),
					"failed to retrieve shared win32 memory handle", false)
#else
		VkMemoryGetFdInfoKHR get_fd_handle {
			.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
			.pNext = nullptr,
			.memory = mem,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
		};
		VK_CALL_RET(vk_ctx.vulkan_get_memory_fd(vulkan_dev, &get_fd_handle, &shared_handle),
					"failed to retrieve shared fd memory handle", false)
#endif
	}
	
	return false;
}

static COMPUTE_IMAGE_TYPE compute_vulkan_image_type(const vulkan_image::external_vulkan_image_info& info, const COMPUTE_MEMORY_FLAG flags) {
	COMPUTE_IMAGE_TYPE type { COMPUTE_IMAGE_TYPE::NONE };
	
	// start with the base format
	type |= (info.image_base_type & (COMPUTE_IMAGE_TYPE::__DIM_MASK |
									 COMPUTE_IMAGE_TYPE::__CHANNELS_MASK |
									 COMPUTE_IMAGE_TYPE::FLAG_ARRAY |
									 COMPUTE_IMAGE_TYPE::FLAG_BUFFER |
									 COMPUTE_IMAGE_TYPE::FLAG_CUBE |
									 COMPUTE_IMAGE_TYPE::FLAG_DEPTH |
									 COMPUTE_IMAGE_TYPE::FLAG_STENCIL |
									 COMPUTE_IMAGE_TYPE::FLAG_MSAA));
	
	// handle the pixel format
	const auto img_type = vulkan_image::image_type_from_vulkan_format(info.format);
	if (!img_type) {
		log_error("unsupported image format: %X", info.format);
		return COMPUTE_IMAGE_TYPE::NONE;
	}
	type |= *img_type;
	
	// handle read/write flags
	if (has_flag<COMPUTE_MEMORY_FLAG::READ>(flags)) type |= COMPUTE_IMAGE_TYPE::READ;
	if (has_flag<COMPUTE_MEMORY_FLAG::WRITE>(flags)) type |= COMPUTE_IMAGE_TYPE::WRITE;
	if (!has_flag<COMPUTE_MEMORY_FLAG::READ>(flags) && !has_flag<COMPUTE_MEMORY_FLAG::WRITE>(flags)) {
		// assume read/write if no flags are set
		type |= COMPUTE_IMAGE_TYPE::READ_WRITE;
	}
	
	// check if this is a render target
	if ((info.access_mask & VK_ACCESS_INPUT_ATTACHMENT_READ_BIT) != 0 ||
		(info.access_mask & VK_ACCESS_COLOR_ATTACHMENT_READ_BIT) != 0 ||
		(info.access_mask & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) != 0 ||
		(info.access_mask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT) != 0 ||
		(info.access_mask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0 ||
		(info.access_mask & VK_ACCESS_COLOR_ATTACHMENT_READ_NONCOHERENT_BIT_EXT) != 0) {
		type |= COMPUTE_IMAGE_TYPE::FLAG_RENDER_TARGET;
	}
	if (info.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
		info.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
		info.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
		info.layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL ||
		info.layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL) {
		type |= COMPUTE_IMAGE_TYPE::FLAG_RENDER_TARGET;
	}
	
	// TODO: handle/check mip-mapping
	// type |= COMPUTE_IMAGE_TYPE::FLAG_MIPMAPPED;
	
	return type;
}

vulkan_image::vulkan_image(const compute_queue& cqueue, const external_vulkan_image_info& external_image, void* host_ptr_, const COMPUTE_MEMORY_FLAG flags_) :
compute_image(cqueue, external_image.dim, compute_vulkan_image_type(external_image, flags_), host_ptr_, flags_, 0, 0, nullptr),
vulkan_memory((const vulkan_device&)cqueue.get_device(), &image), is_external(true) {
	image = external_image.image;
	image_view = external_image.image_view;
	image_info.sampler = nullptr;
	image_info.imageView = image_view;
	image_info.imageLayout = external_image.layout;
	cur_access_mask = external_image.access_mask;
	vk_format = external_image.format;
}

vulkan_image::~vulkan_image() {
	auto vulkan_dev = ((const vulkan_device&)dev).device;
	
	if (!is_external) {
		if(image_view != nullptr) {
			vkDestroyImageView(vulkan_dev, image_view, nullptr);
			image_view = nullptr;
		}
		
		// mip-map image views
		if(is_mip_mapped && (generate_mip_maps || has_flag<COMPUTE_IMAGE_TYPE::WRITE>(image_type))) {
			// only need to destroy all created ones (not up to dev->max_mip_levels)
			for(uint32_t i = 0; i < mip_level_count; ++i) {
				vkDestroyImageView(vulkan_dev, mip_map_image_view[i], nullptr);
			}
		}
		
		if(image != nullptr) {
			vkDestroyImage(vulkan_dev, image, nullptr);
			image = nullptr;
		}
	}
}

bool vulkan_image::zero(const compute_queue& cqueue floor_unused) {
	if(image == nullptr) return false;
	// TODO: implement this
	log_error("vulkan_image::zero not implemented yet");
	return false;
}

void* __attribute__((aligned(128))) vulkan_image::map(const compute_queue& cqueue,
													  const COMPUTE_MEMORY_MAP_FLAG flags_) {
	return vulkan_memory::map(cqueue, flags_, (image_type == shim_image_type ?
											   image_data_size : shim_image_data_size), 0);
}

bool vulkan_image::unmap(const compute_queue& cqueue,
						 void* __attribute__((aligned(128))) mapped_ptr) {
	const auto iter = mappings.find(mapped_ptr);
	if(iter == mappings.end()) {
		log_error("invalid mapped pointer: %X", mapped_ptr);
		return false;
	}
	
	if (!vulkan_memory::unmap(cqueue, mapped_ptr)) {
		return false;
	}
	
	// if we transitioned to a transfer layout during mapping, transition back now
	if (image_info.imageLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
		image_info.imageLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		if (has_flag<COMPUTE_IMAGE_TYPE::READ>(image_type) && !has_flag<COMPUTE_IMAGE_TYPE::WRITE>(image_type)) {
			transition_read(cqueue, nullptr);
		} else {
			transition_write(cqueue, nullptr);
		}
	}
	
	// manually create mip-map chain
	if(generate_mip_maps &&
	   (has_flag<COMPUTE_MEMORY_MAP_FLAG::WRITE>(iter->second.flags) ||
		has_flag<COMPUTE_MEMORY_MAP_FLAG::WRITE_INVALIDATE>(iter->second.flags))) {
		generate_mip_map_chain(cqueue);
	}
	
	return true;
}

void vulkan_image::image_copy_dev_to_host(const compute_queue& cqueue, VkCommandBuffer cmd_buffer, VkBuffer host_buffer) {
	// TODO: mip-mapping, array/layer support, depth/stencil support
	const auto dim_count = image_dim_count(image_type);
	const VkImageSubresourceLayers img_sub_rsrc_layers {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.mipLevel = 0,
		.baseArrayLayer = 0,
		.layerCount = 1,
	};
	const VkBufferImageCopy region {
		.bufferOffset = 0,
		.bufferRowLength = 0, // tightly packed
		.bufferImageHeight = 0, // tightly packed
		.imageSubresource = img_sub_rsrc_layers,
		.imageOffset = { 0, 0, 0 },
		.imageExtent = {
			image_dim.x,
			dim_count >= 2 ? image_dim.y : 1,
			dim_count >= 3 ? image_dim.z : 1,
		},
	};
	// transition to src-optimal, b/c of perf
	transition(cqueue, cmd_buffer,
			   VK_ACCESS_TRANSFER_READ_BIT,
			   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyImageToBuffer(cmd_buffer, image, image_info.imageLayout, host_buffer, 1, &region);
}

void vulkan_image::image_copy_host_to_dev(const compute_queue& cqueue, VkCommandBuffer cmd_buffer, VkBuffer host_buffer, void* data) {
	// TODO: depth/stencil support
	const auto dim_count = image_dim_count(image_type);
	
	// transition to dst-optimal, b/c of perf
	transition(cqueue, cmd_buffer,
			   VK_ACCESS_TRANSFER_WRITE_BIT,
			   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	
	// RGB -> RGBA data conversion if necessary
	if(image_type != shim_image_type) {
		rgb_to_rgba_inplace(image_type, shim_image_type, (uint8_t*)data, generate_mip_maps);
	}
	
	vector<VkBufferImageCopy> regions;
	regions.reserve(mip_level_count);
	uint64_t buffer_offset = 0;
	apply_on_levels([this, &regions, &buffer_offset, &dim_count](const uint32_t& level,
																 const uint4& mip_image_dim,
																 const uint32_t&,
																 const uint32_t& level_data_size) {
		const VkImageSubresourceLayers img_sub_rsrc_layers {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = level,
			.baseArrayLayer = 0,
			.layerCount = layer_count,
		};
		regions.emplace_back(VkBufferImageCopy {
			.bufferOffset = buffer_offset,
			.bufferRowLength = 0, // tightly packed
			.bufferImageHeight = 0, // tightly packed
			.imageSubresource = img_sub_rsrc_layers,
			.imageOffset = { 0, 0, 0 },
			.imageExtent = {
				max(mip_image_dim.x, 1u),
				dim_count >= 2 ? max(mip_image_dim.y, 1u) : 1,
				dim_count >= 3 ? max(mip_image_dim.z, 1u) : 1,
			},
		});
		buffer_offset += level_data_size;
		return true;
	}, shim_image_type);
	
	vkCmdCopyBufferToImage(cmd_buffer, host_buffer, image, image_info.imageLayout,
						   (uint32_t)regions.size(), regions.data());
}

bool vulkan_image::acquire_opengl_object(const compute_queue*) {
	log_error("not supported by vulkan");
	return false;
}

bool vulkan_image::release_opengl_object(const compute_queue*) {
	log_error("not supported by vulkan");
	return false;
}

static VkPipelineStageFlags stage_mask_from_access(const VkAccessFlags& access_mask_in, const VkPipelineStageFlags& stage_mask_in) {
	switch (access_mask_in) {
		case VK_PIPELINE_STAGE_TRANSFER_BIT:
			return VK_PIPELINE_STAGE_TRANSFER_BIT;
		default: break;
	}
	return stage_mask_in;
}

bool vulkan_image::transition(const compute_queue& cqueue,
							  VkCommandBuffer cmd_buffer_,
							  const VkAccessFlags dst_access,
							  const VkImageLayout new_layout,
							  const VkPipelineStageFlags src_stage_mask_in,
							  const VkPipelineStageFlags dst_stage_mask_in,
							  const uint32_t dst_queue_idx) {
	VkImageAspectFlags aspect_mask = 0;
	if(has_flag<COMPUTE_IMAGE_TYPE::FLAG_DEPTH>(image_type)) {
		aspect_mask = VK_IMAGE_ASPECT_DEPTH_BIT;
		if(has_flag<COMPUTE_IMAGE_TYPE::FLAG_STENCIL>(image_type)) {
			aspect_mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	}
	else aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;

	VkPipelineStageFlags src_stage_mask = stage_mask_from_access(cur_access_mask, src_stage_mask_in);
	VkPipelineStageFlags dst_stage_mask = stage_mask_from_access(dst_access, dst_stage_mask_in);
	
	const VkImageMemoryBarrier image_barrier {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = cur_access_mask,
		.dstAccessMask = dst_access,
		.oldLayout = image_info.imageLayout,
		.newLayout = new_layout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, // TODO: use something appropriate here
		.dstQueueFamilyIndex = dst_queue_idx,
		.image = image,
		.subresourceRange = {
			.aspectMask = aspect_mask,
			.baseMipLevel = 0,
			.levelCount = mip_level_count,
			.baseArrayLayer = 0,
			.layerCount = layer_count,
		},
	};
	
	if (cmd_buffer_ == nullptr) {
		const auto& vk_queue = (const vulkan_queue&)cqueue;
		VK_CMD_BLOCK(vk_queue, "image transition", ({
			vkCmdPipelineBarrier(cmd_buffer.cmd_buffer, src_stage_mask, dst_stage_mask,
								 0, 0, nullptr, 0, nullptr, 1, &image_barrier);
		}), true /* always blocking */);
	} else {
		vkCmdPipelineBarrier(cmd_buffer_, src_stage_mask, dst_stage_mask,
							 0, 0, nullptr, 0, nullptr, 1, &image_barrier);
	}
	
	cur_access_mask = dst_access;
	image_info.imageLayout = new_layout;
	update_mip_map_info();
	
	return true;
}

void vulkan_image::transition_read(const compute_queue& cqueue,
								   VkCommandBuffer cmd_buffer,
								   const bool allow_general_layout) {
	// normal images
	if (!has_flag<COMPUTE_IMAGE_TYPE::FLAG_RENDER_TARGET>(image_type)) {
		const VkAccessFlags access_flags = VK_ACCESS_SHADER_READ_BIT;
		if ((cur_access_mask & access_flags) == access_flags) {
			if (image_info.imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
				(allow_general_layout && image_info.imageLayout == VK_IMAGE_LAYOUT_GENERAL)) {
				return;
			}
		}
		transition(cqueue, cmd_buffer, access_flags, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				   VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
	}
	// attachments / render-targets
	else {
		VkImageLayout layout;
		VkAccessFlags access_flags;
		if (!has_flag<COMPUTE_IMAGE_TYPE::FLAG_DEPTH>(image_type)) {
			layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			access_flags = VK_ACCESS_SHADER_READ_BIT;
		} else {
			layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			access_flags = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		}
		if ((cur_access_mask & access_flags) == access_flags) {
			if (image_info.imageLayout == layout ||
				(allow_general_layout && image_info.imageLayout == VK_IMAGE_LAYOUT_GENERAL)) {
				return;
			}
		}
		
		transition(cqueue, cmd_buffer, access_flags, layout,
				   VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
	}
}

void vulkan_image::transition_write(const compute_queue& cqueue, VkCommandBuffer cmd_buffer,
									const bool read_write, const bool is_rt_direct_write, const bool allow_general_layout) {
	// normal images
	if (!has_flag<COMPUTE_IMAGE_TYPE::FLAG_RENDER_TARGET>(image_type) || is_rt_direct_write) {
		VkAccessFlags access_flags = VK_ACCESS_SHADER_WRITE_BIT;
		if (read_write) {
			access_flags |= VK_ACCESS_SHADER_READ_BIT;
		}
		
		if (image_info.imageLayout == VK_IMAGE_LAYOUT_GENERAL &&
			(cur_access_mask & access_flags) == access_flags) {
			return;
		}
		transition(cqueue, cmd_buffer, access_flags, VK_IMAGE_LAYOUT_GENERAL,
				   VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
	}
	// attachments / render-targets
	else {
#if defined(FLOOR_DEBUG)
		if(read_write) {
			log_error("attachment / render-target can't be read-write");
		}
#endif
		
		VkImageLayout layout;
		VkAccessFlags access_flags;
		if (!has_flag<COMPUTE_IMAGE_TYPE::FLAG_DEPTH>(image_type)) {
			layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			access_flags = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		} else {
			layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			access_flags = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		}
		if ((cur_access_mask & access_flags) == access_flags) {
			if (image_info.imageLayout == layout ||
				(allow_general_layout && image_info.imageLayout == VK_IMAGE_LAYOUT_GENERAL)) {
				return;
			}
		}
		
		transition(cqueue, cmd_buffer, access_flags, layout,
				   VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
	}
}

void vulkan_image::update_mip_map_info() {
	// NOTE: sampler is always nullptr, imageView is always the same, so we only need to update the current layout here
	for(auto& info : mip_map_image_info) {
		info.imageLayout = image_info.imageLayout;
	}
}

optional<VkFormat> vulkan_image::vulkan_format_from_image_type(const COMPUTE_IMAGE_TYPE& image_type_) {
	static const unordered_map<COMPUTE_IMAGE_TYPE, VkFormat> format_lut {
		// R
		{ COMPUTE_IMAGE_TYPE::R8UI_NORM, VK_FORMAT_R8_UNORM },
		{ COMPUTE_IMAGE_TYPE::R8I_NORM, VK_FORMAT_R8_SNORM },
		{ COMPUTE_IMAGE_TYPE::R8UI, VK_FORMAT_R8_UINT },
		{ COMPUTE_IMAGE_TYPE::R8I, VK_FORMAT_R8_SINT },
		{ COMPUTE_IMAGE_TYPE::R16UI_NORM, VK_FORMAT_R16_UNORM },
		{ COMPUTE_IMAGE_TYPE::R16I_NORM, VK_FORMAT_R16_SNORM },
		{ COMPUTE_IMAGE_TYPE::R16UI, VK_FORMAT_R16_UINT },
		{ COMPUTE_IMAGE_TYPE::R16I, VK_FORMAT_R16_SINT },
		{ COMPUTE_IMAGE_TYPE::R16F, VK_FORMAT_R16_SFLOAT },
		{ COMPUTE_IMAGE_TYPE::R32UI, VK_FORMAT_R32_UINT },
		{ COMPUTE_IMAGE_TYPE::R32I, VK_FORMAT_R32_SINT },
		{ COMPUTE_IMAGE_TYPE::R32F, VK_FORMAT_R32_SFLOAT },
		// RG
		{ COMPUTE_IMAGE_TYPE::RG8UI_NORM, VK_FORMAT_R8G8_UNORM },
		{ COMPUTE_IMAGE_TYPE::RG8I_NORM, VK_FORMAT_R8G8_SNORM },
		{ COMPUTE_IMAGE_TYPE::RG8UI, VK_FORMAT_R8G8_UINT },
		{ COMPUTE_IMAGE_TYPE::RG8I, VK_FORMAT_R8G8_SINT },
		{ COMPUTE_IMAGE_TYPE::RG16UI_NORM, VK_FORMAT_R16G16_UNORM },
		{ COMPUTE_IMAGE_TYPE::RG16I_NORM, VK_FORMAT_R16G16_SNORM },
		{ COMPUTE_IMAGE_TYPE::RG16UI, VK_FORMAT_R16G16_UINT },
		{ COMPUTE_IMAGE_TYPE::RG16I, VK_FORMAT_R16G16_SINT },
		{ COMPUTE_IMAGE_TYPE::RG16F, VK_FORMAT_R16G16_SFLOAT },
		{ COMPUTE_IMAGE_TYPE::RG32UI, VK_FORMAT_R32G32_UINT },
		{ COMPUTE_IMAGE_TYPE::RG32I, VK_FORMAT_R32G32_SINT },
		{ COMPUTE_IMAGE_TYPE::RG32F, VK_FORMAT_R32G32_SFLOAT },
#if 0 // 3-channel formats are not supported by AMD and NVIDIA, so always use 4-channel formats instead
		// TODO: do this dynamically
		// RGB
		{ COMPUTE_IMAGE_TYPE::RGB8UI_NORM, VK_FORMAT_R8G8B8_UNORM },
		{ COMPUTE_IMAGE_TYPE::RGB8I_NORM, VK_FORMAT_R8G8B8_SNORM },
		{ COMPUTE_IMAGE_TYPE::RGB8UI, VK_FORMAT_R8G8B8_UINT },
		{ COMPUTE_IMAGE_TYPE::RGB8I, VK_FORMAT_R8G8B8_SINT },
		{ COMPUTE_IMAGE_TYPE::RGB16UI_NORM, VK_FORMAT_R16G16B16_UNORM },
		{ COMPUTE_IMAGE_TYPE::RGB16I_NORM, VK_FORMAT_R16G16B16_SNORM },
		{ COMPUTE_IMAGE_TYPE::RGB16UI, VK_FORMAT_R16G16B16_UINT },
		{ COMPUTE_IMAGE_TYPE::RGB16I, VK_FORMAT_R16G16B16_SINT },
		{ COMPUTE_IMAGE_TYPE::RGB16F, VK_FORMAT_R16G16B16_SFLOAT },
		{ COMPUTE_IMAGE_TYPE::RGB32UI, VK_FORMAT_R32G32B32_UINT },
		{ COMPUTE_IMAGE_TYPE::RGB32I, VK_FORMAT_R32G32B32_SINT },
		{ COMPUTE_IMAGE_TYPE::RGB32F, VK_FORMAT_R32G32B32_SFLOAT },
		// BGR
		{ COMPUTE_IMAGE_TYPE::BGR8UI_NORM, VK_FORMAT_B8G8R8_UNORM },
		{ COMPUTE_IMAGE_TYPE::BGR8I_NORM, VK_FORMAT_B8G8R8_SNORM },
		{ COMPUTE_IMAGE_TYPE::BGR8UI, VK_FORMAT_B8G8R8_UINT },
		{ COMPUTE_IMAGE_TYPE::BGR8I, VK_FORMAT_B8G8R8_SINT },
#else
		// RGB
		{ COMPUTE_IMAGE_TYPE::RGB8UI_NORM, VK_FORMAT_R8G8B8A8_UNORM },
		{ COMPUTE_IMAGE_TYPE::RGB8I_NORM, VK_FORMAT_R8G8B8A8_SNORM },
		{ COMPUTE_IMAGE_TYPE::RGB8UI, VK_FORMAT_R8G8B8A8_UINT },
		{ COMPUTE_IMAGE_TYPE::RGB8I, VK_FORMAT_R8G8B8A8_SINT },
		{ COMPUTE_IMAGE_TYPE::RGB16UI_NORM, VK_FORMAT_R16G16B16A16_UNORM },
		{ COMPUTE_IMAGE_TYPE::RGB16I_NORM, VK_FORMAT_R16G16B16A16_SNORM },
		{ COMPUTE_IMAGE_TYPE::RGB16UI, VK_FORMAT_R16G16B16A16_UINT },
		{ COMPUTE_IMAGE_TYPE::RGB16I, VK_FORMAT_R16G16B16A16_SINT },
		{ COMPUTE_IMAGE_TYPE::RGB16F, VK_FORMAT_R16G16B16A16_SFLOAT },
		{ COMPUTE_IMAGE_TYPE::RGB32UI, VK_FORMAT_R32G32B32A32_UINT },
		{ COMPUTE_IMAGE_TYPE::RGB32I, VK_FORMAT_R32G32B32A32_SINT },
		{ COMPUTE_IMAGE_TYPE::RGB32F, VK_FORMAT_R32G32B32A32_SFLOAT },
		// BGR
		{ COMPUTE_IMAGE_TYPE::BGR8UI_NORM, VK_FORMAT_B8G8R8A8_UNORM },
		{ COMPUTE_IMAGE_TYPE::BGR8I_NORM, VK_FORMAT_B8G8R8A8_SNORM },
		{ COMPUTE_IMAGE_TYPE::BGR8UI, VK_FORMAT_B8G8R8A8_UINT },
		{ COMPUTE_IMAGE_TYPE::BGR8I, VK_FORMAT_B8G8R8A8_SINT },
#endif
		// RGBA
		{ COMPUTE_IMAGE_TYPE::RGBA8UI_NORM, VK_FORMAT_R8G8B8A8_UNORM },
		{ COMPUTE_IMAGE_TYPE::RGBA8I_NORM, VK_FORMAT_R8G8B8A8_SNORM },
		{ COMPUTE_IMAGE_TYPE::RGBA8UI, VK_FORMAT_R8G8B8A8_UINT },
		{ COMPUTE_IMAGE_TYPE::RGBA8I, VK_FORMAT_R8G8B8A8_SINT },
		{ COMPUTE_IMAGE_TYPE::RGBA16UI_NORM, VK_FORMAT_R16G16B16A16_UNORM },
		{ COMPUTE_IMAGE_TYPE::RGBA16I_NORM, VK_FORMAT_R16G16B16A16_SNORM },
		{ COMPUTE_IMAGE_TYPE::RGBA16UI, VK_FORMAT_R16G16B16A16_UINT },
		{ COMPUTE_IMAGE_TYPE::RGBA16I, VK_FORMAT_R16G16B16A16_SINT },
		{ COMPUTE_IMAGE_TYPE::RGBA16F, VK_FORMAT_R16G16B16A16_SFLOAT },
		{ COMPUTE_IMAGE_TYPE::RGBA32UI, VK_FORMAT_R32G32B32A32_UINT },
		{ COMPUTE_IMAGE_TYPE::RGBA32I, VK_FORMAT_R32G32B32A32_SINT },
		{ COMPUTE_IMAGE_TYPE::RGBA32F, VK_FORMAT_R32G32B32A32_SFLOAT },
		// BGRA
		{ COMPUTE_IMAGE_TYPE::BGRA8UI_NORM, VK_FORMAT_B8G8R8A8_UNORM },
		{ COMPUTE_IMAGE_TYPE::BGRA8I_NORM, VK_FORMAT_B8G8R8A8_SNORM },
		{ COMPUTE_IMAGE_TYPE::BGRA8UI, VK_FORMAT_B8G8R8A8_UINT },
		{ COMPUTE_IMAGE_TYPE::BGRA8I, VK_FORMAT_B8G8R8A8_SINT },
		{ COMPUTE_IMAGE_TYPE::BGRA8UI_NORM | COMPUTE_IMAGE_TYPE::FLAG_SRGB, VK_FORMAT_B8G8R8A8_SRGB },
		// ABGR
		{ COMPUTE_IMAGE_TYPE::ABGR8UI_NORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32 },
		{ COMPUTE_IMAGE_TYPE::ABGR8I_NORM, VK_FORMAT_A8B8G8R8_SNORM_PACK32 },
		{ COMPUTE_IMAGE_TYPE::ABGR8UI, VK_FORMAT_A8B8G8R8_UINT_PACK32 },
		{ COMPUTE_IMAGE_TYPE::ABGR8I, VK_FORMAT_A8B8G8R8_SINT_PACK32 },
		{ COMPUTE_IMAGE_TYPE::A2BGR10UI_NORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
		{ COMPUTE_IMAGE_TYPE::A2BGR10UI, VK_FORMAT_A2B10G10R10_UINT_PACK32 },
		// ARGB
		{ COMPUTE_IMAGE_TYPE::A2RGB10UI_NORM, VK_FORMAT_A2R10G10B10_UNORM_PACK32 },
		{ COMPUTE_IMAGE_TYPE::A2RGB10UI, VK_FORMAT_A2R10G10B10_UINT_PACK32 },
		// depth / depth+stencil
		{ (COMPUTE_IMAGE_TYPE::UINT |
		   COMPUTE_IMAGE_TYPE::CHANNELS_1 |
		   COMPUTE_IMAGE_TYPE::FORMAT_16 |
		   COMPUTE_IMAGE_TYPE::FLAG_DEPTH), VK_FORMAT_D16_UNORM },
		{ (COMPUTE_IMAGE_TYPE::UINT |
		   COMPUTE_IMAGE_TYPE::CHANNELS_1 |
		   COMPUTE_IMAGE_TYPE::FORMAT_16_8 |
		   COMPUTE_IMAGE_TYPE::FLAG_DEPTH |
		   COMPUTE_IMAGE_TYPE::FLAG_STENCIL), VK_FORMAT_D16_UNORM_S8_UINT },
		{ (COMPUTE_IMAGE_TYPE::FLOAT |
		   COMPUTE_IMAGE_TYPE::CHANNELS_1 |
		   COMPUTE_IMAGE_TYPE::FORMAT_32 |
		   COMPUTE_IMAGE_TYPE::FLAG_DEPTH), VK_FORMAT_D32_SFLOAT },
		{ (COMPUTE_IMAGE_TYPE::UINT |
		   COMPUTE_IMAGE_TYPE::CHANNELS_2 |
		   COMPUTE_IMAGE_TYPE::FORMAT_24_8 |
		   COMPUTE_IMAGE_TYPE::FLAG_DEPTH |
		   COMPUTE_IMAGE_TYPE::FLAG_STENCIL), VK_FORMAT_D24_UNORM_S8_UINT },
		{ (COMPUTE_IMAGE_TYPE::FLOAT |
		   COMPUTE_IMAGE_TYPE::CHANNELS_2 |
		   COMPUTE_IMAGE_TYPE::FORMAT_32_8 |
		   COMPUTE_IMAGE_TYPE::FLAG_DEPTH |
		   COMPUTE_IMAGE_TYPE::FLAG_STENCIL), VK_FORMAT_D32_SFLOAT_S8_UINT },
		// BC1 - BC3
		{ COMPUTE_IMAGE_TYPE::BC1_RGB, VK_FORMAT_BC1_RGB_UNORM_BLOCK },
		{ COMPUTE_IMAGE_TYPE::BC1_RGB_SRGB, VK_FORMAT_BC1_RGB_SRGB_BLOCK },
		{ COMPUTE_IMAGE_TYPE::BC1_RGBA, VK_FORMAT_BC1_RGBA_UNORM_BLOCK },
		{ COMPUTE_IMAGE_TYPE::BC1_RGBA_SRGB, VK_FORMAT_BC1_RGBA_SRGB_BLOCK },
		{ COMPUTE_IMAGE_TYPE::BC2_RGBA, VK_FORMAT_BC2_UNORM_BLOCK },
		{ COMPUTE_IMAGE_TYPE::BC2_RGBA_SRGB, VK_FORMAT_BC2_SRGB_BLOCK },
		{ COMPUTE_IMAGE_TYPE::BC3_RGBA, VK_FORMAT_BC3_UNORM_BLOCK },
		{ COMPUTE_IMAGE_TYPE::BC3_RGBA_SRGB, VK_FORMAT_BC3_SRGB_BLOCK },
		// BC4 - BC5
		{ COMPUTE_IMAGE_TYPE::RGTC_RI, VK_FORMAT_BC4_SNORM_BLOCK },
		{ COMPUTE_IMAGE_TYPE::RGTC_RUI, VK_FORMAT_BC4_UNORM_BLOCK },
		{ COMPUTE_IMAGE_TYPE::RGTC_RGI, VK_FORMAT_BC5_SNORM_BLOCK },
		{ COMPUTE_IMAGE_TYPE::RGTC_RGUI, VK_FORMAT_BC5_UNORM_BLOCK },
		// BC6 - BC7
		{ COMPUTE_IMAGE_TYPE::BPTC_RGBHF, VK_FORMAT_BC6H_SFLOAT_BLOCK },
		{ COMPUTE_IMAGE_TYPE::BPTC_RGBUHF, VK_FORMAT_BC6H_UFLOAT_BLOCK },
		{ COMPUTE_IMAGE_TYPE::BPTC_RGBA, VK_FORMAT_BC7_UNORM_BLOCK },
		{ COMPUTE_IMAGE_TYPE::BPTC_RGBA_SRGB, VK_FORMAT_BC7_SRGB_BLOCK },
		// PVRTC formats
		// NOTE: not to be confused with PVRTC version 2, here: PVRTC1 == RGB, PVRTC2 == RGBA
		{ COMPUTE_IMAGE_TYPE::PVRTC_RGB2, VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG },
		{ COMPUTE_IMAGE_TYPE::PVRTC_RGB4, VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG },
		{ COMPUTE_IMAGE_TYPE::PVRTC_RGBA2, VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG },
		{ COMPUTE_IMAGE_TYPE::PVRTC_RGBA4, VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG },
		{ COMPUTE_IMAGE_TYPE::PVRTC_RGB2_SRGB, VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG },
		{ COMPUTE_IMAGE_TYPE::PVRTC_RGB4_SRGB, VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG },
		{ COMPUTE_IMAGE_TYPE::PVRTC_RGBA2_SRGB, VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG },
		{ COMPUTE_IMAGE_TYPE::PVRTC_RGBA4_SRGB, VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG },
	};
	const auto vulkan_format = format_lut.find(image_type_ & (COMPUTE_IMAGE_TYPE::__DATA_TYPE_MASK |
															  COMPUTE_IMAGE_TYPE::__CHANNELS_MASK |
															  COMPUTE_IMAGE_TYPE::__COMPRESSION_MASK |
															  COMPUTE_IMAGE_TYPE::__FORMAT_MASK |
															  COMPUTE_IMAGE_TYPE::__LAYOUT_MASK |
															  COMPUTE_IMAGE_TYPE::FLAG_NORMALIZED |
															  COMPUTE_IMAGE_TYPE::FLAG_DEPTH |
															  COMPUTE_IMAGE_TYPE::FLAG_STENCIL |
															  COMPUTE_IMAGE_TYPE::FLAG_SRGB));
	if (vulkan_format == end(format_lut)) {
		return {};
	}
	return vulkan_format->second;
}

optional<COMPUTE_IMAGE_TYPE> vulkan_image::image_type_from_vulkan_format(const VkFormat& format_) {
	static const unordered_map<VkFormat, COMPUTE_IMAGE_TYPE> format_lut {
		// R
		{ VK_FORMAT_R8_UNORM, COMPUTE_IMAGE_TYPE::R8UI_NORM },
		{ VK_FORMAT_R8_SNORM, COMPUTE_IMAGE_TYPE::R8I_NORM },
		{ VK_FORMAT_R8_UINT, COMPUTE_IMAGE_TYPE::R8UI },
		{ VK_FORMAT_R8_SINT, COMPUTE_IMAGE_TYPE::R8I },
		{ VK_FORMAT_R16_UNORM, COMPUTE_IMAGE_TYPE::R16UI_NORM },
		{ VK_FORMAT_R16_SNORM, COMPUTE_IMAGE_TYPE::R16I_NORM },
		{ VK_FORMAT_R16_UINT, COMPUTE_IMAGE_TYPE::R16UI },
		{ VK_FORMAT_R16_SINT, COMPUTE_IMAGE_TYPE::R16I },
		{ VK_FORMAT_R16_SFLOAT, COMPUTE_IMAGE_TYPE::R16F },
		{ VK_FORMAT_R32_UINT, COMPUTE_IMAGE_TYPE::R32UI },
		{ VK_FORMAT_R32_SINT, COMPUTE_IMAGE_TYPE::R32I },
		{ VK_FORMAT_R32_SFLOAT, COMPUTE_IMAGE_TYPE::R32F },
		// RG
		{ VK_FORMAT_R8G8_UNORM, COMPUTE_IMAGE_TYPE::RG8UI_NORM },
		{ VK_FORMAT_R8G8_SNORM, COMPUTE_IMAGE_TYPE::RG8I_NORM },
		{ VK_FORMAT_R8G8_UINT, COMPUTE_IMAGE_TYPE::RG8UI },
		{ VK_FORMAT_R8G8_SINT, COMPUTE_IMAGE_TYPE::RG8I },
		{ VK_FORMAT_R16G16_UNORM, COMPUTE_IMAGE_TYPE::RG16UI_NORM },
		{ VK_FORMAT_R16G16_SNORM, COMPUTE_IMAGE_TYPE::RG16I_NORM },
		{ VK_FORMAT_R16G16_UINT, COMPUTE_IMAGE_TYPE::RG16UI },
		{ VK_FORMAT_R16G16_SINT, COMPUTE_IMAGE_TYPE::RG16I },
		{ VK_FORMAT_R16G16_SFLOAT, COMPUTE_IMAGE_TYPE::RG16F },
		{ VK_FORMAT_R32G32_UINT, COMPUTE_IMAGE_TYPE::RG32UI },
		{ VK_FORMAT_R32G32_SINT, COMPUTE_IMAGE_TYPE::RG32I },
		{ VK_FORMAT_R32G32_SFLOAT, COMPUTE_IMAGE_TYPE::RG32F },
		// NOTE: no RGB here
		// RGBA
		{ VK_FORMAT_R8G8B8A8_UNORM, COMPUTE_IMAGE_TYPE::RGBA8UI_NORM },
		{ VK_FORMAT_R8G8B8A8_SNORM, COMPUTE_IMAGE_TYPE::RGBA8I_NORM },
		{ VK_FORMAT_R8G8B8A8_UINT, COMPUTE_IMAGE_TYPE::RGBA8UI },
		{ VK_FORMAT_R8G8B8A8_SINT, COMPUTE_IMAGE_TYPE::RGBA8I },
		{ VK_FORMAT_R16G16B16A16_UNORM, COMPUTE_IMAGE_TYPE::RGBA16UI_NORM },
		{ VK_FORMAT_R16G16B16A16_SNORM, COMPUTE_IMAGE_TYPE::RGBA16I_NORM },
		{ VK_FORMAT_R16G16B16A16_UINT, COMPUTE_IMAGE_TYPE::RGBA16UI },
		{ VK_FORMAT_R16G16B16A16_SINT, COMPUTE_IMAGE_TYPE::RGBA16I },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, COMPUTE_IMAGE_TYPE::RGBA16F },
		{ VK_FORMAT_R32G32B32A32_UINT, COMPUTE_IMAGE_TYPE::RGBA32UI },
		{ VK_FORMAT_R32G32B32A32_SINT, COMPUTE_IMAGE_TYPE::RGBA32I },
		{ VK_FORMAT_R32G32B32A32_SFLOAT, COMPUTE_IMAGE_TYPE::RGBA32F },
		// BGRA
		{ VK_FORMAT_B8G8R8A8_UNORM, COMPUTE_IMAGE_TYPE::BGRA8UI_NORM },
		{ VK_FORMAT_B8G8R8A8_SNORM, COMPUTE_IMAGE_TYPE::BGRA8I_NORM },
		{ VK_FORMAT_B8G8R8A8_UINT, COMPUTE_IMAGE_TYPE::BGRA8UI },
		{ VK_FORMAT_B8G8R8A8_SINT, COMPUTE_IMAGE_TYPE::BGRA8I },
		{ VK_FORMAT_B8G8R8A8_SRGB, COMPUTE_IMAGE_TYPE::BGRA8UI_NORM | COMPUTE_IMAGE_TYPE::FLAG_SRGB },
		// ABGR
		{ VK_FORMAT_A8B8G8R8_UNORM_PACK32, COMPUTE_IMAGE_TYPE::ABGR8UI_NORM },
		{ VK_FORMAT_A8B8G8R8_SNORM_PACK32, COMPUTE_IMAGE_TYPE::ABGR8I_NORM },
		{ VK_FORMAT_A8B8G8R8_UINT_PACK32, COMPUTE_IMAGE_TYPE::ABGR8UI },
		{ VK_FORMAT_A8B8G8R8_SINT_PACK32, COMPUTE_IMAGE_TYPE::ABGR8I },
		{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, COMPUTE_IMAGE_TYPE::A2BGR10UI_NORM },
		{ VK_FORMAT_A2B10G10R10_UINT_PACK32, COMPUTE_IMAGE_TYPE::A2BGR10UI },
		// ARGB
		{ VK_FORMAT_A2R10G10B10_UNORM_PACK32, COMPUTE_IMAGE_TYPE::A2RGB10UI_NORM },
		{ VK_FORMAT_A2R10G10B10_UINT_PACK32, COMPUTE_IMAGE_TYPE::A2RGB10UI },
		// depth / depth+stencil
		{ VK_FORMAT_D16_UNORM, (COMPUTE_IMAGE_TYPE::UINT |
								COMPUTE_IMAGE_TYPE::CHANNELS_1 |
								COMPUTE_IMAGE_TYPE::FORMAT_16 |
								COMPUTE_IMAGE_TYPE::FLAG_DEPTH) },
		{ VK_FORMAT_D16_UNORM_S8_UINT, (COMPUTE_IMAGE_TYPE::UINT |
										COMPUTE_IMAGE_TYPE::CHANNELS_1 |
										COMPUTE_IMAGE_TYPE::FORMAT_16_8 |
										COMPUTE_IMAGE_TYPE::FLAG_DEPTH |
										COMPUTE_IMAGE_TYPE::FLAG_STENCIL) },
		{ VK_FORMAT_D32_SFLOAT, (COMPUTE_IMAGE_TYPE::FLOAT |
								 COMPUTE_IMAGE_TYPE::CHANNELS_1 |
								 COMPUTE_IMAGE_TYPE::FORMAT_32 |
								 COMPUTE_IMAGE_TYPE::FLAG_DEPTH) },
		{ VK_FORMAT_D24_UNORM_S8_UINT, (COMPUTE_IMAGE_TYPE::UINT |
										COMPUTE_IMAGE_TYPE::CHANNELS_2 |
										COMPUTE_IMAGE_TYPE::FORMAT_24_8 |
										COMPUTE_IMAGE_TYPE::FLAG_DEPTH |
										COMPUTE_IMAGE_TYPE::FLAG_STENCIL) },
		{ VK_FORMAT_D32_SFLOAT_S8_UINT, (COMPUTE_IMAGE_TYPE::FLOAT |
										 COMPUTE_IMAGE_TYPE::CHANNELS_2 |
										 COMPUTE_IMAGE_TYPE::FORMAT_32_8 |
										 COMPUTE_IMAGE_TYPE::FLAG_DEPTH |
										 COMPUTE_IMAGE_TYPE::FLAG_STENCIL) },
		// BC1 - BC3
		{ VK_FORMAT_BC1_RGB_UNORM_BLOCK, COMPUTE_IMAGE_TYPE::BC1_RGB },
		{ VK_FORMAT_BC1_RGB_SRGB_BLOCK, COMPUTE_IMAGE_TYPE::BC1_RGB_SRGB },
		{ VK_FORMAT_BC1_RGBA_UNORM_BLOCK, COMPUTE_IMAGE_TYPE::BC1_RGBA },
		{ VK_FORMAT_BC1_RGBA_SRGB_BLOCK, COMPUTE_IMAGE_TYPE::BC1_RGBA_SRGB },
		{ VK_FORMAT_BC2_UNORM_BLOCK, COMPUTE_IMAGE_TYPE::BC2_RGBA },
		{ VK_FORMAT_BC2_SRGB_BLOCK, COMPUTE_IMAGE_TYPE::BC2_RGBA_SRGB },
		{ VK_FORMAT_BC3_UNORM_BLOCK, COMPUTE_IMAGE_TYPE::BC3_RGBA },
		{ VK_FORMAT_BC3_SRGB_BLOCK, COMPUTE_IMAGE_TYPE::BC3_RGBA_SRGB },
		// BC4 - BC5
		{ VK_FORMAT_BC4_SNORM_BLOCK, COMPUTE_IMAGE_TYPE::RGTC_RI },
		{ VK_FORMAT_BC4_UNORM_BLOCK, COMPUTE_IMAGE_TYPE::RGTC_RUI },
		{ VK_FORMAT_BC5_SNORM_BLOCK, COMPUTE_IMAGE_TYPE::RGTC_RGI },
		{ VK_FORMAT_BC5_UNORM_BLOCK, COMPUTE_IMAGE_TYPE::RGTC_RGUI },
		// BC6 - BC7
		{ VK_FORMAT_BC6H_SFLOAT_BLOCK, COMPUTE_IMAGE_TYPE::BPTC_RGBHF },
		{ VK_FORMAT_BC6H_UFLOAT_BLOCK, COMPUTE_IMAGE_TYPE::BPTC_RGBUHF },
		{ VK_FORMAT_BC7_UNORM_BLOCK, COMPUTE_IMAGE_TYPE::BPTC_RGBA },
		{ VK_FORMAT_BC7_SRGB_BLOCK, COMPUTE_IMAGE_TYPE::BPTC_RGBA_SRGB },
		// PVRTC formats
		// NOTE: not to be confused with PVRTC version 2, here: PVRTC1 == RGB, PVRTC2 == RGBA
		{ VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG, COMPUTE_IMAGE_TYPE::PVRTC_RGB2 },
		{ VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG, COMPUTE_IMAGE_TYPE::PVRTC_RGB4 },
		{ VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG, COMPUTE_IMAGE_TYPE::PVRTC_RGBA2 },
		{ VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG, COMPUTE_IMAGE_TYPE::PVRTC_RGBA4 },
		{ VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG, COMPUTE_IMAGE_TYPE::PVRTC_RGB2_SRGB },
		{ VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG, COMPUTE_IMAGE_TYPE::PVRTC_RGB4_SRGB },
		{ VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG, COMPUTE_IMAGE_TYPE::PVRTC_RGBA2_SRGB },
		{ VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG, COMPUTE_IMAGE_TYPE::PVRTC_RGBA4_SRGB },
	};
	const auto img_type = format_lut.find(format_);
	if (img_type == end(format_lut)) {
		return {};
	}
	return img_type->second;
}

void vulkan_image::update_with_external_vulkan_state(const VkImageLayout& layout, const VkAccessFlags& access) {
	image_info.imageLayout = layout;
	cur_access_mask = access;
	update_mip_map_info();
}

void vulkan_image::set_debug_label(const string& label) {
	compute_memory::set_debug_label(label);
	((const vulkan_compute*)device.context)->set_vulkan_debug_label(device, VK_OBJECT_TYPE_IMAGE, uint64_t(image), label);
	if (image_view) {
		((const vulkan_compute*)device.context)->set_vulkan_debug_label(device, VK_OBJECT_TYPE_IMAGE_VIEW, uint64_t(image_view), label);
	}
}

#endif
