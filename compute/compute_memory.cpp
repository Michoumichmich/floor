/*
 *  Flo's Open libRary (floor)
 *  Copyright (C) 2004 - 2016 Florian Ziesche
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

#include <floor/compute/compute_memory.hpp>
#include <floor/core/logger.hpp>

static constexpr COMPUTE_MEMORY_FLAG handle_memory_flags(COMPUTE_MEMORY_FLAG flags, const uint32_t opengl_type) {
	// opengl sharing handling
	if(has_flag<COMPUTE_MEMORY_FLAG::OPENGL_SHARING>(flags)) {
		// check if specified opengl type is valid
		if(opengl_type == 0) {
			log_error("OpenGL sharing has been set, but no OpenGL object type has been specified!");
		}
		// TODO: check for known opengl types? how to deal with unknown ones?
		
		// clear out USE_HOST_MEMORY flag if it is set
		if(has_flag<COMPUTE_MEMORY_FLAG::USE_HOST_MEMORY>(flags)) {
			flags &= ~COMPUTE_MEMORY_FLAG::USE_HOST_MEMORY;
		}
	}
	
	// handle read/write flags
	if((flags & COMPUTE_MEMORY_FLAG::READ_WRITE) == COMPUTE_MEMORY_FLAG::NONE) {
		// neither read nor write is set -> set read/write
		flags |= COMPUTE_MEMORY_FLAG::READ_WRITE;
	}
	
	// handle host read/write flags
	if((flags & COMPUTE_MEMORY_FLAG::HOST_READ_WRITE) == COMPUTE_MEMORY_FLAG::NONE &&
	   has_flag<COMPUTE_MEMORY_FLAG::USE_HOST_MEMORY>(flags)) {
		// can't be using host memory and declaring that the host doesn't access the memory
		log_error("USE_HOST_MEMORY specified, but host read/write flags set to NONE!");
		flags |= COMPUTE_MEMORY_FLAG::HOST_READ_WRITE;
	}
	
	return flags;
}

compute_memory::compute_memory(const void* device,
							   void* host_ptr_,
							   const COMPUTE_MEMORY_FLAG flags_,
							   const uint32_t opengl_type_,
							   const uint32_t external_gl_object_) :
dev(device), host_ptr(host_ptr_), flags(handle_memory_flags(flags_, opengl_type_)),
has_external_gl_object(external_gl_object_ != 0), opengl_type(opengl_type_),
gl_object(has_external_gl_object ? external_gl_object_ : 0) {
	if((flags_ & COMPUTE_MEMORY_FLAG::READ_WRITE) == COMPUTE_MEMORY_FLAG::NONE) {
		log_error("memory must be read-only, write-only or read-write!");
	}
	if(has_flag<COMPUTE_MEMORY_FLAG::USE_HOST_MEMORY>(flags_) &&
	   has_flag<COMPUTE_MEMORY_FLAG::OPENGL_SHARING>(flags_)) {
		log_error("USE_HOST_MEMORY and OPENGL_SHARING are mutually exclusive!");
	}
}

compute_memory::~compute_memory() {}

void compute_memory::_lock() {
	lock.lock();
}

void compute_memory::_unlock() {
	lock.unlock();
}
