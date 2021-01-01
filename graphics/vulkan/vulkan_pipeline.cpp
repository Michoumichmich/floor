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

#include <floor/graphics/vulkan/vulkan_pipeline.hpp>

#if !defined(FLOOR_NO_VULKAN)

#include <floor/compute/vulkan/vulkan_device.hpp>
#include <floor/compute/vulkan/vulkan_kernel.hpp>

static unique_ptr<vulkan_pass> create_vulkan_base_pass_desc(const render_pipeline_description& pipeline_desc,
															const vector<unique_ptr<compute_device>>& devices,
															const bool is_multi_view) {
	render_pass_description base_pass_desc {};
	if (is_multi_view) {
		// disable automatic transformation, since we already have made it
		base_pass_desc.automatic_multi_view_handling = false;
	}
	for (const auto& color_att : pipeline_desc.color_attachments) {
		base_pass_desc.attachments.emplace_back(render_pass_description::attachment_desc_t {
			.format = color_att.format
			// NOTE: load op, store op, clear color/depth do not matter (any combination is compatible)
		});
	}
	if (pipeline_desc.depth_attachment.format != COMPUTE_IMAGE_TYPE::NONE) {
		base_pass_desc.attachments.emplace_back(render_pass_description::attachment_desc_t {
			.format = pipeline_desc.depth_attachment.format
		});
	}
	auto vulkan_base_pass = make_unique<vulkan_pass>(base_pass_desc, devices, is_multi_view);
	if (!vulkan_base_pass || !vulkan_base_pass->is_valid()) {
		log_error("failed to create%s Vulkan base pass for pipeline", (is_multi_view ? " multi-view" : ""));
		return {};
	}
	return vulkan_base_pass;
}

static bool create_vulkan_pipeline(vulkan_pipeline::vulkan_pipeline_state_t& state,
								   const vulkan_pass& vulkan_base_pass,
								   const render_pipeline_description& pipeline_desc,
								   const vulkan_device& vk_dev,
								   const vulkan_kernel::vulkan_kernel_entry* vk_vs_entry,
								   const vulkan_kernel::vulkan_kernel_entry* vk_fs_entry,
								   const bool is_multi_view) {
	state.vs_entry = vk_vs_entry;
	state.fs_entry = vk_fs_entry;

	// create the pipeline layout
	vector<VkDescriptorSetLayout> desc_set_layouts {
		vk_dev.fixed_sampler_desc_set_layout,
		vk_vs_entry->desc_set_layout
	};
	if (vk_fs_entry != nullptr) {
		desc_set_layouts.emplace_back(vk_fs_entry->desc_set_layout);
	}
	const VkPipelineLayoutCreateInfo pipeline_layout_info {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.setLayoutCount = uint32_t(desc_set_layouts.size()),
		.pSetLayouts = desc_set_layouts.data(),
		.pushConstantRangeCount = 0,
		.pPushConstantRanges = nullptr,
	};
	VK_CALL_RET(vkCreatePipelineLayout(vk_dev.device, &pipeline_layout_info, nullptr, &state.layout),
				"failed to create pipeline layout", false)

	// setup the pipeline
	const VkPipelineVertexInputStateCreateInfo vertex_input_state {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		// unnecessary when using SSBOs
		.vertexBindingDescriptionCount = 0,
		.pVertexBindingDescriptions = nullptr,
		.vertexAttributeDescriptionCount = 0,
		.pVertexAttributeDescriptions = nullptr,
	};
	const VkPipelineInputAssemblyStateCreateInfo input_assembly_state {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.topology = vulkan_pipeline::vulkan_primitive_topology_from_primitive(pipeline_desc.primitive),
		.primitiveRestartEnable = false,
	};
	const VkViewport viewport {
		.x = 0.0f,
		.y = 0.0f,
		.width = (float)pipeline_desc.viewport.x,
		.height = (float)pipeline_desc.viewport.y,
		.minDepth = pipeline_desc.depth.range.x,
		.maxDepth = pipeline_desc.depth.range.y,
	};
	const VkRect2D scissor_rect {
		// NOTE: Vulkan uses signed integers for the offset, but doesn't actually it to be < 0
		.offset = { int(pipeline_desc.scissor.offset.x), int(pipeline_desc.scissor.offset.y) },
		.extent = { pipeline_desc.scissor.extent.x, pipeline_desc.scissor.extent.y },
	};
	const VkPipelineViewportStateCreateInfo viewport_state {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.viewportCount = 1,
		.pViewports = &viewport,
		.scissorCount = 1,
		.pScissors = &scissor_rect,
	};
	const VkPipelineRasterizationStateCreateInfo raster_state {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.depthClampEnable = false,
		.rasterizerDiscardEnable = false,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = vulkan_pipeline::vulkan_cull_mode_from_cull_mode(pipeline_desc.cull_mode),
		.frontFace = vulkan_pipeline::vulkan_front_face_from_front_face(pipeline_desc.front_face),
		.depthBiasEnable = false,
		.depthBiasConstantFactor = 0.0f,
		.depthBiasClamp = 0.0f,
		.depthBiasSlopeFactor = 0.0f,
		.lineWidth = 1.0f,
	};
	const VkPipelineMultisampleStateCreateInfo multisample_state {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		.sampleShadingEnable = false,
		.minSampleShading = 0.0f,
		.pSampleMask = nullptr,
		.alphaToCoverageEnable = false,
		.alphaToOneEnable = false,
	};

	// set color attachments
	vector<VkPipelineColorBlendAttachmentState> color_blend_attachment_states;
	for (const auto& color_att : pipeline_desc.color_attachments) {
		if (color_att.format == COMPUTE_IMAGE_TYPE::NONE) {
			log_error("color attachment image type must not be NONE!"); // TODO: -> move to prior validity check
			return false;
		}

		VkColorComponentFlags mask = 0;
		if (color_att.blend.write_mask.x) {
			mask |= VK_COLOR_COMPONENT_R_BIT;
		}
		if (color_att.blend.write_mask.y) {
			mask |= VK_COLOR_COMPONENT_G_BIT;
		}
		if (color_att.blend.write_mask.z) {
			mask |= VK_COLOR_COMPONENT_B_BIT;
		}
		if (color_att.blend.write_mask.w) {
			mask |= VK_COLOR_COMPONENT_A_BIT;
		}

		color_blend_attachment_states.emplace_back(VkPipelineColorBlendAttachmentState {
			.blendEnable = color_att.blend.enable,
			.srcColorBlendFactor = vulkan_pipeline::vulkan_blend_factor_from_blend_factor(color_att.blend.src_color_factor),
			.dstColorBlendFactor = vulkan_pipeline::vulkan_blend_factor_from_blend_factor(color_att.blend.dst_color_factor),
			.colorBlendOp = vulkan_pipeline::vulkan_blend_op_from_blend_op(color_att.blend.color_blend_op),
			.srcAlphaBlendFactor = vulkan_pipeline::vulkan_blend_factor_from_blend_factor(color_att.blend.src_alpha_factor),
			.dstAlphaBlendFactor = vulkan_pipeline::vulkan_blend_factor_from_blend_factor(color_att.blend.dst_alpha_factor),
			.alphaBlendOp = vulkan_pipeline::vulkan_blend_op_from_blend_op(color_att.blend.alpha_blend_op),
			.colorWriteMask = mask,
		});
	}
	const VkPipelineColorBlendStateCreateInfo color_blend_state {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.logicOpEnable = false,
		.logicOp = VK_LOGIC_OP_CLEAR,
		.attachmentCount = uint32_t(color_blend_attachment_states.size()),
		.pAttachments = (!color_blend_attachment_states.empty() ? color_blend_attachment_states.data() : nullptr),
		.blendConstants = {
			pipeline_desc.blend.constant_color.x,
			pipeline_desc.blend.constant_color.y,
			pipeline_desc.blend.constant_color.z,
			pipeline_desc.blend.constant_alpha
		},
	};

	// set optional depth attachment
	VkPipelineDepthStencilStateCreateInfo depth_stencil_state;
	const bool has_depth_attachment = (pipeline_desc.depth_attachment.format != COMPUTE_IMAGE_TYPE::NONE);
	if (has_depth_attachment) {
		depth_stencil_state = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.depthTestEnable = (pipeline_desc.depth.compare != DEPTH_COMPARE::ALWAYS),
			.depthWriteEnable = pipeline_desc.depth.write,
			.depthCompareOp = vulkan_pipeline::vulkan_compare_op_from_depth_compare(pipeline_desc.depth.compare),
			.depthBoundsTestEnable = false,
			.stencilTestEnable = false,
			.front = {},
			.back = {},
			.minDepthBounds = 0.0f,
			.maxDepthBounds = 0.0f,
		};
	}

	// create pipeline
	array<VkPipelineShaderStageCreateInfo, 2> stages {};
	stages[0] = vk_vs_entry->stage_info;
	if (vk_fs_entry != nullptr) {
		stages[1] = vk_fs_entry->stage_info;
	}

	const auto render_pass = vulkan_base_pass.get_vulkan_render_pass(vk_dev, is_multi_view);
	if (render_pass == nullptr) {
		log_error("no base render pass for device %s", vk_dev.name);
		return false;
	}

	// allow dynamic change of viewport and scissor
	const array dyn_state_arr {
		VkDynamicState::VK_DYNAMIC_STATE_VIEWPORT,
		VkDynamicState::VK_DYNAMIC_STATE_SCISSOR,
	};
	const VkPipelineDynamicStateCreateInfo dyn_state {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.dynamicStateCount = size(dyn_state_arr),
		.pDynamicStates = &dyn_state_arr[0],
	};
	const VkGraphicsPipelineCreateInfo gfx_pipeline_info {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.stageCount = (vk_fs_entry != nullptr ? 2 : 1),
		.pStages = &stages[0],
		.pVertexInputState = &vertex_input_state,
		.pInputAssemblyState = &input_assembly_state,
		.pTessellationState = nullptr,
		.pViewportState = &viewport_state,
		.pRasterizationState = &raster_state,
		.pMultisampleState = &multisample_state,
		.pDepthStencilState = (has_depth_attachment ? &depth_stencil_state : nullptr),
		.pColorBlendState = &color_blend_state,
		.pDynamicState = &dyn_state,
		.layout = state.layout,
		.renderPass = render_pass,
		.subpass = 0,
		.basePipelineHandle = nullptr,
		.basePipelineIndex = 0,
	};
	VK_CALL_RET(vkCreateGraphicsPipelines(vk_dev.device, nullptr, 1, &gfx_pipeline_info, nullptr, &state.pipeline),
				"failed to create pipeline", false)

	return true;
}

vulkan_pipeline::vulkan_pipeline(const render_pipeline_description& pipeline_desc_, const vector<unique_ptr<compute_device>>& devices,
								 const bool with_multi_view_support) :
graphics_pipeline(pipeline_desc_, with_multi_view_support) {
	const bool create_sv_pipeline = is_single_view_capable();
	const bool create_mv_pipeline = is_multi_view_capable();

	// Vulkan requires an actual render pass for pipeline creation, it is however allowed to use a compatible render pass later on
	// -> create a base render pass for this pipeline, since we can't access the actual corresponding render pass here (and there might be multiple ones)
	sv_vulkan_base_pass = (create_sv_pipeline ? create_vulkan_base_pass_desc(pipeline_desc, devices, false) : nullptr);
	mv_vulkan_base_pass = (create_mv_pipeline ? create_vulkan_base_pass_desc((multi_view_pipeline_desc ? *multi_view_pipeline_desc : pipeline_desc),
																			 devices, true) : nullptr);
	
	// now create the actual pipeline(s)
	const auto vk_vs = (const vulkan_kernel*)pipeline_desc.vertex_shader;
	const auto vk_fs = (const vulkan_kernel*)pipeline_desc.fragment_shader;
	
	for (const auto& dev : devices) {
		const auto& vk_dev = (const vulkan_device&)*dev;
		const auto vk_vs_entry = (const vulkan_kernel::vulkan_kernel_entry*)vk_vs->get_kernel_entry(*dev);
		const auto vk_fs_entry = (vk_fs != nullptr ? (const vulkan_kernel::vulkan_kernel_entry*)vk_fs->get_kernel_entry(*dev) : nullptr);

		vulkan_pipeline_entry_t entry {};
		if (create_sv_pipeline) {
			if (!create_vulkan_pipeline(entry.single_view_pipeline, *sv_vulkan_base_pass, pipeline_desc,
										vk_dev, vk_vs_entry, vk_fs_entry, false)) {
				return;
			}
		}
		if (create_mv_pipeline) {
			if (!create_vulkan_pipeline(entry.multi_view_pipeline, *mv_vulkan_base_pass,
										(multi_view_pipeline_desc ? *multi_view_pipeline_desc : pipeline_desc), vk_dev, vk_vs_entry, vk_fs_entry, true)) {
				return;
			}
		}
		
		pipelines.insert_or_assign(*dev, entry);
	}
	
	// success
	valid = true;
}

vulkan_pipeline::~vulkan_pipeline() {
	// TODO: implement this
}

const vulkan_pipeline::vulkan_pipeline_state_t* vulkan_pipeline::get_vulkan_pipeline_state(const compute_device& dev, const bool get_multi_view) const {
	const auto ret = pipelines.get(dev);
	return !ret.first ? nullptr : (!get_multi_view ? &ret.second->second.single_view_pipeline : &ret.second->second.multi_view_pipeline);
}

VkPrimitiveTopology vulkan_pipeline::vulkan_primitive_topology_from_primitive(const PRIMITIVE& primitive) {
	switch (primitive) {
		case PRIMITIVE::POINT:
			return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
		case PRIMITIVE::LINE:
			return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		case PRIMITIVE::TRIANGLE:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		case PRIMITIVE::TRIANGLE_STRIP:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	}
}

VkCullModeFlagBits vulkan_pipeline::vulkan_cull_mode_from_cull_mode(const CULL_MODE& cull_mode) {
	switch (cull_mode) {
		case CULL_MODE::NONE:
			return VK_CULL_MODE_NONE;
		case CULL_MODE::BACK:
			return VK_CULL_MODE_BACK_BIT;
		case CULL_MODE::FRONT:
			return VK_CULL_MODE_FRONT_BIT;
	}
}

VkFrontFace vulkan_pipeline::vulkan_front_face_from_front_face(const FRONT_FACE& front_face) {
	switch (front_face) {
		case FRONT_FACE::CLOCKWISE:
			return VK_FRONT_FACE_CLOCKWISE;
		case FRONT_FACE::COUNTER_CLOCKWISE:
			return VK_FRONT_FACE_COUNTER_CLOCKWISE;
	}
}

VkBlendFactor vulkan_pipeline::vulkan_blend_factor_from_blend_factor(const BLEND_FACTOR& blend_factor) {
	switch (blend_factor) {
		case BLEND_FACTOR::ZERO:
			return VK_BLEND_FACTOR_ZERO;
		case BLEND_FACTOR::ONE:
			return VK_BLEND_FACTOR_ONE;
			
		case BLEND_FACTOR::SRC_COLOR:
			return VK_BLEND_FACTOR_SRC_COLOR;
		case BLEND_FACTOR::ONE_MINUS_SRC_COLOR:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case BLEND_FACTOR::DST_COLOR:
			return VK_BLEND_FACTOR_DST_COLOR;
		case BLEND_FACTOR::ONE_MINUS_DST_COLOR:
			return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
			
		case BLEND_FACTOR::SRC_ALPHA:
			return VK_BLEND_FACTOR_SRC_ALPHA;
		case BLEND_FACTOR::ONE_MINUS_SRC_ALPHA:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case BLEND_FACTOR::DST_ALPHA:
			return VK_BLEND_FACTOR_DST_ALPHA;
		case BLEND_FACTOR::ONE_MINUS_DST_ALPHA:
			return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case BLEND_FACTOR::SRC_ALPHA_SATURATE:
			return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
			
		case BLEND_FACTOR::BLEND_COLOR:
			return VK_BLEND_FACTOR_CONSTANT_COLOR;
		case BLEND_FACTOR::ONE_MINUS_BLEND_COLOR:
			return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
		case BLEND_FACTOR::BLEND_ALPHA:
			return VK_BLEND_FACTOR_CONSTANT_ALPHA;
		case BLEND_FACTOR::ONE_MINUE_BLEND_ALPHA:
			return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
	}
}

VkBlendOp vulkan_pipeline::vulkan_blend_op_from_blend_op(const BLEND_OP& blend_op) {
	switch (blend_op) {
		case BLEND_OP::ADD:
			return VK_BLEND_OP_ADD;
		case BLEND_OP::SUB:
			return VK_BLEND_OP_SUBTRACT;
		case BLEND_OP::REV_SUB:
			return VK_BLEND_OP_REVERSE_SUBTRACT;
		case BLEND_OP::MIN:
			return VK_BLEND_OP_MIN;
		case BLEND_OP::MAX:
			return VK_BLEND_OP_MAX;
	}
}

VkCompareOp vulkan_pipeline::vulkan_compare_op_from_depth_compare(const DEPTH_COMPARE& depth_compare) {
	switch (depth_compare) {
		case DEPTH_COMPARE::NEVER:
			return VK_COMPARE_OP_NEVER;
		case DEPTH_COMPARE::LESS:
			return VK_COMPARE_OP_LESS;
		case DEPTH_COMPARE::EQUAL:
			return VK_COMPARE_OP_EQUAL;
		case DEPTH_COMPARE::LESS_OR_EQUAL:
			return VK_COMPARE_OP_LESS_OR_EQUAL;
		case DEPTH_COMPARE::GREATER:
			return VK_COMPARE_OP_GREATER;
		case DEPTH_COMPARE::NOT_EQUAL:
			return VK_COMPARE_OP_NOT_EQUAL;
		case DEPTH_COMPARE::GREATER_OR_EQUAL:
			return VK_COMPARE_OP_GREATER_OR_EQUAL;
		case DEPTH_COMPARE::ALWAYS:
			return VK_COMPARE_OP_ALWAYS;
	}
}

#endif
