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

#ifndef __FLOOR_CONF_HPP__
#define __FLOOR_CONF_HPP__

// when building on osx/ios, always disable opencl
// for vulkan, put the disable behind a big ol' hack, so that we can at least get syntax
// highlighting/completion when vulkan headers are installed (will build, but won't link though)
#if defined(__APPLE__)
#if !__has_include(<floor/floor/vulkan_testing.hpp>)
#define FLOOR_NO_VULKAN 1
#else
#define FLOOR_VULKAN_TESTING 1
#include <floor/floor/vulkan_testing.hpp>
#endif
#define FLOOR_NO_OPENCL 1
#endif

// if defined, this disables cuda support
//#define FLOOR_NO_CUDA 1

// if defined, this disables host compute support
//#define FLOOR_NO_HOST_COMPUTE 1

// if defined, this disables opencl support
//#define FLOOR_NO_OPENCL 1

#if !defined(FLOOR_VULKAN_TESTING)
// if defined, this disables vulkan support
//#define FLOOR_NO_VULKAN 1
#endif

// if defined, this disables metal support
#if defined(__APPLE__)
//#define FLOOR_NO_METAL 1
#else
#define FLOOR_NO_METAL 1
#endif

// if defined, this disables openal support
//#define FLOOR_NO_OPENAL 1

// if defined, this disables VR support
//#define FLOOR_NO_VR 1

// if defined, this disables network support
//#define FLOOR_NO_NET 1

// if defined, this will use extern templates for specific template classes (vector*, matrix, etc.)
// and instantiate them for various basic types (float, int, ...)
// NOTE: don't enable this for compute (these won't compile the necessary .cpp files)
#if !defined(FLOOR_COMPUTE) || (defined(FLOOR_COMPUTE_HOST) && !defined(FLOOR_COMPUTE_HOST_DEVICE))
#define FLOOR_EXPORT 1
#endif

// use asio standalone and header-only
#if !defined(FLOOR_NO_NET)
#define ASIO_STANDALONE 1
#define ASIO_HEADER_ONLY 1
#define ASIO_NO_EXCEPTIONS 1
#define ASIO_DISABLE_BOOST_THROW_EXCEPTION 1
#define ASIO_DISABLE_BUFFER_DEBUGGING 1
#endif

// no VR support on iOS/macOS
#if defined(__APPLE__) && !defined(FLOOR_NO_VR)
#define FLOOR_NO_VR 1
#endif

#endif
