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

#ifndef __FLOOR_COMPUTE_QUEUE_HPP__
#define __FLOOR_COMPUTE_QUEUE_HPP__

#include <string>
#include <vector>
#include <floor/math/vector_lib.hpp>
#include <floor/compute/compute_kernel_arg.hpp>

FLOOR_PUSH_WARNINGS()
FLOOR_IGNORE_WARNING(weak-vtables)

class compute_device;
class compute_memory;
class compute_kernel;
class indirect_command_pipeline;

class compute_queue {
public:
	//! argument type validity specializations, with pretty error messages
	template <typename T> struct is_valid_arg { static constexpr bool valid() { return true; } };
	
	template <typename T> struct is_compute_memory_pointer : public false_type {};
	template <typename T>
	requires (is_pointer_v<T> &&
			  (is_base_of_v<compute_memory, remove_pointer_t<T>> ||
			   is_base_of_v<argument_buffer, remove_pointer_t<T>>))
	struct is_compute_memory_pointer<T> : public true_type {};
	
	template <typename T>
	requires (is_pointer_v<T> && !is_compute_memory_pointer<T>::value)
	struct is_valid_arg<T> {
		static constexpr bool valid()
		__attribute__((unavailable("raw pointers are not allowed"))) { return false; }
	};
	template <typename T>
	requires is_null_pointer_v<T>
	struct is_valid_arg<T> {
		static constexpr bool valid()
		__attribute__((unavailable("nullptr is not allowed"))) { return false; }
	};
	
	//! checks if an individual argument type is valid
	template <typename T = void>
	static constexpr bool check_arg_types() {
		constexpr const bool is_valid { is_valid_arg<T>::valid() };
		static_assert(is_valid, "invalid argument type");
		return is_valid;
	}
	
	//! checks if all argument types are valid
	template <typename T, typename... Args> requires (sizeof...(Args) != 0)
	static constexpr bool check_arg_types() {
		return (check_arg_types<T>() && check_arg_types<Args...>());
	}
	
public:
	explicit compute_queue(const compute_device& device_) : device(device_) {}
	virtual ~compute_queue() = default;
	
	//! blocks until all currently scheduled work in this queue has been executed
	virtual void finish() const = 0;
	
	//! flushes all scheduled work to the associated device
	virtual void flush() const = 0;
	
	//! implementation specific queue object ptr (cl_command_queue or CUStream, both "struct _ *")
	virtual const void* get_queue_ptr() const = 0;
	virtual void* get_queue_ptr() = 0;
	
	//! enqueues (and executes) the specified kernel into this queue
	template <typename... Args, class work_size_type_global, class work_size_type_local>
	requires ((is_same_v<decay_t<work_size_type_global>, uint1> ||
			   is_same_v<decay_t<work_size_type_global>, uint2> ||
			   is_same_v<decay_t<work_size_type_global>, uint3>) &&
			  is_same_v<decay_t<work_size_type_global>, decay_t<work_size_type_local>>)
	void execute(const compute_kernel& kernel,
				 work_size_type_global&& global_work_size,
				 work_size_type_local&& local_work_size,
				 const Args&... args) const __attribute__((enable_if(check_arg_types<Args...>(), "valid args"))) {
		kernel_execute_forwarder(kernel, false, global_work_size, local_work_size, { args... });
	}
	
	template <typename... Args, class work_size_type_global, class work_size_type_local>
	requires ((is_same_v<decay_t<work_size_type_global>, uint1> ||
			   is_same_v<decay_t<work_size_type_global>, uint2> ||
			   is_same_v<decay_t<work_size_type_global>, uint3>) &&
			  is_same_v<decay_t<work_size_type_global>, decay_t<work_size_type_local>>)
	void execute(const compute_kernel&, work_size_type_global&&, work_size_type_local&&, const Args&...) const
	__attribute__((enable_if(!check_arg_types<Args...>(), "invalid args"), unavailable("invalid kernel argument(s)!")));
	
	template <typename... Args, class work_size_type_global, class work_size_type_local>
	requires ((is_same_v<decay_t<work_size_type_global>, uint1> ||
			   is_same_v<decay_t<work_size_type_global>, uint2> ||
			   is_same_v<decay_t<work_size_type_global>, uint3>) &&
			  is_same_v<decay_t<work_size_type_global>, decay_t<work_size_type_local>>)
	void execute(shared_ptr<compute_kernel>, work_size_type_global&&, work_size_type_local&&, const Args&...) const
	__attribute__((enable_if(!check_arg_types<Args...>(), "invalid args"), unavailable("invalid kernel argument(s)!")));
	
#if !defined(FLOOR_IOS)
	//! enqueues (and executes cooperatively) the specified kernel into this queue
	//! NOTE: the device/backend this is executed on requires "cooperative_kernel_support"
	template <typename... Args, class work_size_type_global, class work_size_type_local>
	requires ((is_same_v<decay_t<work_size_type_global>, uint1> ||
			   is_same_v<decay_t<work_size_type_global>, uint2> ||
			   is_same_v<decay_t<work_size_type_global>, uint3>) &&
			  is_same_v<decay_t<work_size_type_global>, decay_t<work_size_type_local>>)
	void execute_cooperative(const compute_kernel& kernel,
							 work_size_type_global&& global_work_size,
							 work_size_type_local&& local_work_size,
							 const Args&... args) const __attribute__((enable_if(check_arg_types<Args...>(), "valid args"))) {
		kernel_execute_forwarder(kernel, true, global_work_size, local_work_size, { args... });
	}
	
	template <typename... Args, class work_size_type_global, class work_size_type_local>
	requires ((is_same_v<decay_t<work_size_type_global>, uint1> ||
			   is_same_v<decay_t<work_size_type_global>, uint2> ||
			   is_same_v<decay_t<work_size_type_global>, uint3>) &&
			  is_same_v<decay_t<work_size_type_global>, decay_t<work_size_type_local>>)
	void execute_cooperative(const compute_kernel&, work_size_type_global&&, work_size_type_local&&, const Args&...) const
	
	__attribute__((enable_if(!check_arg_types<Args...>(), "invalid args"), unavailable("invalid kernel argument(s)!")));
	template <typename... Args, class work_size_type_global, class work_size_type_local>
	requires ((is_same_v<decay_t<work_size_type_global>, uint1> ||
			   is_same_v<decay_t<work_size_type_global>, uint2> ||
			   is_same_v<decay_t<work_size_type_global>, uint3>) &&
			  is_same_v<decay_t<work_size_type_global>, decay_t<work_size_type_local>>)
	void execute_cooperative(shared_ptr<compute_kernel>, work_size_type_global&&, work_size_type_local&&, const Args&...) const
	__attribute__((enable_if(!check_arg_types<Args...>(), "invalid args"), unavailable("invalid kernel argument(s)!")));
#endif
	
	//! executes the compute commands from an indirect command pipeline
	//! executes #"command_count" commands (or all if ~0u) starting at "command_offset" -> all commands by default
	//! NOTE: the device/backend this is executed on requires "indirect_compute_command_support"
	virtual void execute_indirect(const indirect_command_pipeline& indirect_cmd,
								  const uint32_t command_offset = 0u,
								  const uint32_t command_count = ~0u) const = 0;
	
	//! returns the compute device associated with this queue
	const compute_device& get_device() const { return device; }
	
	//! returns true if this queue has profiling support
	virtual bool has_profiling_support() const {
		return false;
	}
	
	//! starts profiling
	virtual void start_profiling();
	
	//! stops the previously started profiling and returns the elapsed time in microseconds
	virtual uint64_t stop_profiling();
	
protected:
	const compute_device& device;
	uint64_t us_prof_start { 0 };
	
	//! internal forwarders to the actual kernel execution implementations
	void kernel_execute_forwarder(const compute_kernel& kernel,
								  const bool is_cooperative,
								  const uint1& global_size, const uint1& local_size,
								  const vector<compute_kernel_arg>& args) const;
	void kernel_execute_forwarder(const compute_kernel& kernel,
								  const bool is_cooperative,
								  const uint2& global_size, const uint2& local_size,
								  const vector<compute_kernel_arg>& args) const;
	void kernel_execute_forwarder(const compute_kernel& kernel,
								  const bool is_cooperative,
								  const uint3& global_size, const uint3& local_size,
								  const vector<compute_kernel_arg>& args) const;
	
};

FLOOR_POP_WARNINGS()

#endif
