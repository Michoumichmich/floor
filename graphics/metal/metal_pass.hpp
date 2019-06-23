/*
 *  Flo's Open libRary (floor)
 *  Copyright (C) 2004 - 2019 Florian Ziesche
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

#ifndef __FLOOR_GRAPHICS_METAL_METAL_PASS_HPP__
#define __FLOOR_GRAPHICS_METAL_METAL_PASS_HPP__

#include <floor/compute/metal/metal_common.hpp>

#if !defined(FLOOR_NO_METAL)
#include <floor/graphics/graphics_pass.hpp>
#include <Metal/Metal.h>

class metal_pass final : public graphics_pass {
public:
	metal_pass(const render_pass_description& pass_desc);
	virtual ~metal_pass();
	
	//! returns the corresponding MTLLoadAction for the specified LOAD_OP
	static MTLLoadAction metal_load_action_from_load_op(const LOAD_OP& load_op);
	
	//! returns the corresponding MTLStoreAction for the specified STORE_OP
	static MTLStoreAction metal_store_action_from_store_op(const STORE_OP& store_op);
	
	//! creates an encoder for this render pass description
	id <MTLRenderCommandEncoder> create_encoder(id <MTLCommandBuffer> cmd_buffer) const;
	
	//! returns the Metal render pass descriptor
	const MTLRenderPassDescriptor* get_metal_pass_desc() const {
		return mtl_pass_desc;
	}
	
protected:
	MTLRenderPassDescriptor* mtl_pass_desc { nil };
	
};

#endif

#endif
