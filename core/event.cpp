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

#include <floor/core/event.hpp>
#include <floor/floor/floor.hpp>
#include <floor/core/unicode.hpp>
#include <floor/vr/vr_context.hpp>

event::event() : thread_base("event") {
	const uint32_t cur_time { SDL_GetTicks() };
	lm_double_click_timer = cur_time;
	rm_double_click_timer = cur_time;
	mm_double_click_timer = cur_time;
	this->start();
}

event::~event() {
	// finish/kill the event thread before deleting any shared event data
	finish();
}

void event::run() {
	// user events are handled "asynchronously", so they don't
	// interfere with other (internal) events or engine code
	
	// copy/move user events to a processing queue, so the queue can
	// still be used in the other (main) event thread
	user_queue_lock.lock();
	user_event_queue_processing.swap(user_event_queue);
	user_queue_lock.unlock();
	
	handle_user_events();
}

/*! handles the sdl events
 */
void event::handle_events() {
	// always acquire the gl context for internal handlers, since these are very likely to modify gl data
	floor::acquire_context();
	
	// internal engine event handler
	const int coord_scale = (floor::get_hidpi() ? int(floor::get_scale_factor()) : 1);
	const auto coord_scalef = float(coord_scale);
	while(SDL_PollEvent(&event_handle)) {
		const auto event_type = event_handle.type;
		const uint32_t cur_ticks = SDL_GetTicks();
		
		if(event_type == SDL_MOUSEBUTTONDOWN ||
		   event_type == SDL_MOUSEBUTTONUP) {
			// mouse event handling
			const int2 mouse_coord { event_handle.button.x * coord_scale, event_handle.button.y * coord_scale };
#if defined(FLOOR_SDL_PRESSURE) // currently only enabled in my patched/customized SDL lib
			const float pressure = event_handle.button.pressure;
#else
			const float pressure = 0.0f;
#endif
			
			switch(event_type) {
				default: break;
				case SDL_MOUSEBUTTONDOWN: {
					switch(event_handle.button.button) {
						case SDL_BUTTON_LEFT:
							if(event_handle.button.state == SDL_PRESSED) {
								handle_event(EVENT_TYPE::MOUSE_LEFT_DOWN,
											 make_shared<mouse_left_down_event>(cur_ticks, mouse_coord, pressure));
							}
							break;
						case SDL_BUTTON_RIGHT:
							if(event_handle.button.state == SDL_PRESSED) {
								handle_event(EVENT_TYPE::MOUSE_RIGHT_DOWN,
											 make_shared<mouse_right_down_event>(cur_ticks, mouse_coord, pressure));
							}
							break;
						case SDL_BUTTON_MIDDLE:
							if(event_handle.button.state == SDL_PRESSED) {
								handle_event(EVENT_TYPE::MOUSE_MIDDLE_DOWN,
											 make_shared<mouse_middle_down_event>(cur_ticks, mouse_coord, pressure));
							}
							break;
						default: break;
					}
				}
				break;
				case SDL_MOUSEBUTTONUP: {
					switch(event_handle.button.button) {
						case SDL_BUTTON_LEFT:
							if(event_handle.button.state == SDL_RELEASED) {
								handle_event(EVENT_TYPE::MOUSE_LEFT_UP,
											 make_shared<mouse_left_up_event>(cur_ticks, mouse_coord, pressure));
								
								if(cur_ticks - lm_double_click_timer < ldouble_click_time) {
									// emit a double click event
									handle_event(EVENT_TYPE::MOUSE_LEFT_DOUBLE_CLICK,
												 make_shared<mouse_left_double_click_event>(
													cur_ticks,
													prev_events[EVENT_TYPE::MOUSE_LEFT_DOWN],
													prev_events[EVENT_TYPE::MOUSE_LEFT_UP]));
								}
								else {
									// only emit a normal click event
									handle_event(EVENT_TYPE::MOUSE_LEFT_CLICK,
												 make_shared<mouse_left_click_event>(
													cur_ticks,
													prev_events[EVENT_TYPE::MOUSE_LEFT_DOWN],
													prev_events[EVENT_TYPE::MOUSE_LEFT_UP]));
								}
								
								lm_double_click_timer = cur_ticks;
							}
							break;
						case SDL_BUTTON_RIGHT:
							if(event_handle.button.state == SDL_RELEASED) {
								handle_event(EVENT_TYPE::MOUSE_RIGHT_UP,
											 make_shared<mouse_right_up_event>(cur_ticks, mouse_coord, pressure));
								
								if(cur_ticks - rm_double_click_timer < rdouble_click_time) {
									// emit a double click event
									handle_event(EVENT_TYPE::MOUSE_RIGHT_DOUBLE_CLICK,
												 make_shared<mouse_right_double_click_event>(
													cur_ticks,
													prev_events[EVENT_TYPE::MOUSE_RIGHT_DOWN],
													prev_events[EVENT_TYPE::MOUSE_RIGHT_UP]));
								}
								else {
									// only emit a normal click event
									handle_event(EVENT_TYPE::MOUSE_RIGHT_CLICK,
												 make_shared<mouse_right_click_event>(
													cur_ticks,
													prev_events[EVENT_TYPE::MOUSE_RIGHT_DOWN],
													prev_events[EVENT_TYPE::MOUSE_RIGHT_UP]));
								}
								
								rm_double_click_timer = cur_ticks;
							}
							break;
						case SDL_BUTTON_MIDDLE:
							if(event_handle.button.state == SDL_RELEASED) {
								handle_event(EVENT_TYPE::MOUSE_MIDDLE_UP,
											 make_shared<mouse_middle_up_event>(cur_ticks, mouse_coord, pressure));
								
								if(SDL_GetTicks() - mm_double_click_timer < mdouble_click_time) {
									// emit a double click event
									handle_event(EVENT_TYPE::MOUSE_MIDDLE_DOUBLE_CLICK,
												 make_shared<mouse_middle_double_click_event>(
													cur_ticks,
													prev_events[EVENT_TYPE::MOUSE_MIDDLE_DOWN],
													prev_events[EVENT_TYPE::MOUSE_MIDDLE_UP]));
								}
								else {
									// only emit a normal click event
									handle_event(EVENT_TYPE::MOUSE_MIDDLE_CLICK,
												 make_shared<mouse_middle_click_event>(
													cur_ticks,
													prev_events[EVENT_TYPE::MOUSE_MIDDLE_DOWN],
													prev_events[EVENT_TYPE::MOUSE_MIDDLE_UP]));
								}
								
								mm_double_click_timer = cur_ticks;
							}
							break;
						default: break;
					}
				}
				break;
			}
		}
		else if(event_type == SDL_MOUSEMOTION ||
				event_type == SDL_MOUSEWHEEL) {
			switch(event_type) {
				case SDL_MOUSEMOTION: {
					const int2 abs_pos { event_handle.motion.x * coord_scale, event_handle.motion.y * coord_scale };
					const int2 rel_move { event_handle.motion.xrel * coord_scale, event_handle.motion.yrel * coord_scale };
#if defined(FLOOR_SDL_PRESSURE) // currently only enabled in my patched/customized SDL lib
					const float pressure = event_handle.motion.pressure;
#else
					const float pressure = 0.0f;
#endif
					handle_event(EVENT_TYPE::MOUSE_MOVE,
								 make_shared<mouse_move_event>(cur_ticks, abs_pos, rel_move, pressure));
				}
				break;
				case SDL_MOUSEWHEEL: {
					// this sdl event contains no mouse button coordinate, so we need to get it ourselves
					int2 mouse_coord;
					SDL_GetMouseState(&mouse_coord.x, &mouse_coord.y);
					if(event_handle.wheel.y > 0) {
						handle_event(EVENT_TYPE::MOUSE_WHEEL_UP,
									 make_shared<mouse_wheel_up_event>(cur_ticks,
																	   mouse_coord,
																	   event_handle.wheel.y));
					}
					else if(event_handle.wheel.y < 0) {
						const auto abs_wheel_move = (uint32_t)abs(event_handle.wheel.y);
						handle_event(EVENT_TYPE::MOUSE_WHEEL_DOWN,
									 make_shared<mouse_wheel_down_event>(cur_ticks,
																		 mouse_coord,
																		 abs_wheel_move));
					}
				}
				break;
				default: break;
			}
		}
		else if(event_type == SDL_FINGERDOWN ||
				event_type == SDL_FINGERUP ||
				event_type == SDL_FINGERMOTION) {
			// touch event handling
			const float2 finger_coord { event_handle.tfinger.x * coord_scalef, event_handle.tfinger.y * coord_scalef };
			const float pressure = event_handle.tfinger.pressure;
			const auto finger_id = event_handle.tfinger.fingerId;
			
			if(event_type == SDL_FINGERDOWN) {
				if(event_handle.tfinger.type == SDL_FINGERDOWN) {
					handle_event(EVENT_TYPE::FINGER_DOWN,
								 make_shared<finger_down_event>(cur_ticks, finger_coord, pressure, finger_id));
				}
			}
			else if(event_type == SDL_FINGERUP) {
				if(event_handle.tfinger.type == SDL_FINGERUP) {
					handle_event(EVENT_TYPE::FINGER_UP,
								 make_shared<finger_up_event>(cur_ticks, finger_coord, pressure, finger_id));
				}
			}
			else if(event_type == SDL_FINGERMOTION) {
				if(event_handle.tfinger.type == SDL_FINGERMOTION) {
					const float2 rel_move { event_handle.tfinger.dx, event_handle.tfinger.dy };
					handle_event(EVENT_TYPE::FINGER_MOVE,
								 make_shared<finger_move_event>(cur_ticks, finger_coord, rel_move, pressure, finger_id));
				}
			}
		}
		else {
			// key, etc. event handling
			switch(event_type) {
				case SDL_KEYUP:
					handle_event(EVENT_TYPE::KEY_UP,
								 make_shared<key_up_event>(cur_ticks, event_handle.key.keysym.sym));
					break;
				case SDL_KEYDOWN:
					handle_event(EVENT_TYPE::KEY_DOWN,
								 make_shared<key_up_event>(cur_ticks, event_handle.key.keysym.sym));
					break;
				case SDL_TEXTINPUT: {
					string text;
					for(size_t i = 0; i < SDL_TEXTINPUTEVENT_TEXT_SIZE; i++) {
						if(event_handle.text.text[i] == 0) break;
						text += event_handle.text.text[i];
					}
					const auto codes = unicode::utf8_to_unicode(text);
					for(const auto& code : codes) {
						handle_event(EVENT_TYPE::UNICODE_INPUT,
									 make_shared<unicode_input_event>(cur_ticks, code));
					}
				}
				break;
				case SDL_WINDOWEVENT:
					if(event_handle.window.event == SDL_WINDOWEVENT_RESIZED) {
						const size2 new_size((size_t)event_handle.window.data1, (size_t)event_handle.window.data2);
						handle_event(EVENT_TYPE::WINDOW_RESIZE,
									 make_shared<window_resize_event>(cur_ticks, new_size));
					}
					break;
				case SDL_QUIT:
					handle_event(EVENT_TYPE::QUIT, make_shared<quit_event>(cur_ticks));
					break;
				case SDL_CLIPBOARDUPDATE:
					handle_event(EVENT_TYPE::CLIPBOARD_UPDATE,
								 make_shared<clipboard_update_event>(cur_ticks, SDL_HasClipboardText() ? SDL_GetClipboardText() : ""));
					break;
				default: break;
			}
		}
	}

#if !defined(FLOOR_NO_VR)
	// handle VR events
	if (vr_ctx != nullptr) {
		const auto vr_events = vr_ctx->update_input();
		for (const auto& vr_event : vr_events) {
			handle_event(((const vr_event_base<EVENT_TYPE::__VR_CONTROLLER_EVENT>*)vr_event.get())->type, vr_event);
		}
	}
#endif
	
	floor::release_context();
}

uint2 event::get_mouse_pos() const {
	uint2 pos;
	SDL_GetMouseState((int*)&pos.x, (int*)&pos.y);
	pos *= (floor::get_hidpi() ? uint32_t(floor::get_scale_factor()) : 1u);
	return pos;
}

void event::set_ldouble_click_time(uint32_t dctime) {
	ldouble_click_time = dctime;
}

void event::set_rdouble_click_time(uint32_t dctime) {
	rdouble_click_time = dctime;
}

void event::set_mdouble_click_time(uint32_t dctime) {
	mdouble_click_time = dctime;
}

void event::add_event_handler(handler& handler_, EVENT_TYPE type) {
	int cas_zero = 0;
	while(!handlers_lock.compare_exchange_strong(cas_zero, handlers_locked)) {
		this_thread::yield();
	}
	handlers.insert(pair<EVENT_TYPE, handler&>(type, handler_));
	handlers_lock = 0;
}

void event::add_internal_event_handler(handler& handler_, EVENT_TYPE type) {
	int cas_zero = 0;
	while(!handlers_lock.compare_exchange_strong(cas_zero, handlers_locked)) {
		this_thread::yield();
	}
	internal_handlers.insert(pair<EVENT_TYPE, handler&>(type, handler_));
	handlers_lock = 0;
}

void event::add_event(const EVENT_TYPE type, shared_ptr<event_object> obj) {
	// for now, just pass it through
	handle_event(type, obj); // TODO: add to queue and handle in handle_events (-> required for correct mt)
}

void event::handle_event(const EVENT_TYPE& type, shared_ptr<event_object> obj) {
	// set/override last event for this type
	prev_events[type] = obj;
	
	// call internal event handlers directly
	int cur_hl = handlers_lock;
	while(cur_hl == handlers_locked ||
		  !handlers_lock.compare_exchange_strong(cur_hl, cur_hl+1)) {
		this_thread::yield();
		cur_hl = handlers_lock;
	}
	
	const auto range = internal_handlers.equal_range(type);
	for(auto iter = range.first; iter != range.second; iter++) {
		// ignore return value for now (TODO: actually use this?)
		iter->second(type, obj);
	}
	
	handlers_lock--;
	
	// push to user event queue (these will be handled later on)
	user_queue_lock.lock();
	user_event_queue.push(make_pair(type, obj));
	user_queue_lock.unlock();
}

void event::handle_user_events() {
	while(!user_event_queue_processing.empty()) {
		// pop next event
		pair<EVENT_TYPE, shared_ptr<event_object>> evt = user_event_queue_processing.front();
		user_event_queue_processing.pop();
		
		// call user event handlers
		int cur_hl = handlers_lock;
		while(cur_hl == handlers_locked ||
			  !handlers_lock.compare_exchange_strong(cur_hl, cur_hl+1)) {
			this_thread::yield();
			cur_hl = handlers_lock;
		}
		
		const auto range = handlers.equal_range(evt.first);
		for(auto iter = range.first; iter != range.second; iter++) {
			iter->second(evt.first, evt.second);
		}
		
		handlers_lock--;
	}
}

void event::remove_event_handler(const handler& handler_) {
	int cas_zero = 0;
	while(!handlers_lock.compare_exchange_strong(cas_zero, handlers_locked)) {
		this_thread::yield();
	}
	
	for(auto handler_iter = handlers.cbegin(); handler_iter != handlers.cend(); ) {
		// good old pointer comparison ...
		if(&handler_iter->second == &handler_) {
			handler_iter = handlers.erase(handler_iter);
		}
		else ++handler_iter;
	}
	for(auto handler_iter = internal_handlers.cbegin(); handler_iter != internal_handlers.cend(); ) {
		if(&handler_iter->second == &handler_) {
			handler_iter = internal_handlers.erase(handler_iter);
		}
		else ++handler_iter;
	}
	
	handlers_lock = 0;
}

void event::remove_event_types_from_handler(const handler& handler_, const set<EVENT_TYPE>& types) {
	int cas_zero = 0;
	while(!handlers_lock.compare_exchange_strong(cas_zero, handlers_locked)) {
		this_thread::yield();
	}
	
	for(const auto& type : types) {
		const auto range_0 = handlers.equal_range(type);
		for(auto iter = range_0.first; iter != range_0.second; iter++) {
			if(&iter->second == &handler_) {
				handlers.erase(iter);
				break;
			}
		}
		
		const auto range_1 = internal_handlers.equal_range(type);
		for(auto iter = range_1.first; iter != range_1.second; iter++) {
			if(&iter->second == &handler_) {
				internal_handlers.erase(iter);
				break;
			}
		}
	}
	
	handlers_lock = 0;
}
