/*
 * Copyright © 2013 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#include "../shared/os-compatibility.h"
#include "compositor.h"

static void
empty_region(pixman_region32_t *region)
{
	pixman_region32_fini(region);
	pixman_region32_init(region);
}

static void unbind_resource(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

void
weston_seat_repick(struct weston_seat *seat)
{
	const struct weston_pointer *pointer = seat->pointer;

	if (pointer == NULL)
		return;

	pointer->grab->interface->focus(seat->pointer->grab);
}

static void
weston_compositor_idle_inhibit(struct weston_compositor *compositor)
{
	weston_compositor_wake(compositor);
	compositor->idle_inhibit++;
}

static void
weston_compositor_idle_release(struct weston_compositor *compositor)
{
	compositor->idle_inhibit--;
	weston_compositor_wake(compositor);
}

static void
lose_pointer_focus(struct wl_listener *listener, void *data)
{
	struct weston_pointer *pointer =
		container_of(listener, struct weston_pointer, focus_listener);

	pointer->focus_resource = NULL;
}

static void
lose_keyboard_focus(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard =
		container_of(listener, struct weston_keyboard, focus_listener);

	keyboard->focus_resource = NULL;
}

static void
lose_touch_focus(struct wl_listener *listener, void *data)
{
	struct weston_touch *touch =
		container_of(listener, struct weston_touch, focus_listener);

	touch->focus_resource = NULL;
}

static void
default_grab_focus(struct weston_pointer_grab *grab)
{
	struct weston_pointer *pointer = grab->pointer;
	struct weston_view *view;
	wl_fixed_t sx, sy;

	if (pointer->button_count > 0)
		return;

	view = weston_compositor_pick_view(pointer->seat->compositor,
					   pointer->x, pointer->y,
					   &sx, &sy);

	if (pointer->focus != view)
		weston_pointer_set_focus(pointer, view, sx, sy);
}

static void
default_grab_motion(struct weston_pointer_grab *grab, uint32_t time)
{
	struct weston_pointer *pointer = grab->pointer;
	wl_fixed_t sx, sy;

	if (pointer->focus_resource) {
		weston_view_from_global_fixed(pointer->focus,
					      pointer->x, pointer->y,
					      &sx, &sy);
		wl_pointer_send_motion(pointer->focus_resource, time, sx, sy);
	}
}

static void
default_grab_button(struct weston_pointer_grab *grab,
		    uint32_t time, uint32_t button, uint32_t state_w)
{
	struct weston_pointer *pointer = grab->pointer;
	struct weston_compositor *compositor = pointer->seat->compositor;
	struct weston_view *view;
	struct wl_resource *resource;
	uint32_t serial;
	enum wl_pointer_button_state state = state_w;
	struct wl_display *display = compositor->wl_display;
	wl_fixed_t sx, sy;

	resource = pointer->focus_resource;
	if (resource) {
		serial = wl_display_next_serial(display);
		wl_pointer_send_button(resource, serial, time, button, state_w);
	}

	if (pointer->button_count == 0 &&
	    state == WL_POINTER_BUTTON_STATE_RELEASED) {
		view = weston_compositor_pick_view(compositor,
						   pointer->x, pointer->y,
						   &sx, &sy);

		weston_pointer_set_focus(pointer, view, sx, sy);
	}
}

static const struct weston_pointer_grab_interface
				default_pointer_grab_interface = {
	default_grab_focus,
	default_grab_motion,
	default_grab_button
};

static void
default_grab_touch_down(struct weston_touch_grab *grab, uint32_t time,
			int touch_id, wl_fixed_t sx, wl_fixed_t sy)
{
	struct weston_touch *touch = grab->touch;
	struct wl_display *display = touch->seat->compositor->wl_display;
	uint32_t serial;

	if (touch->focus_resource && touch->focus) {
		serial = wl_display_next_serial(display);
		wl_touch_send_down(touch->focus_resource, serial, time,
				   touch->focus->surface->resource,
				   touch_id, sx, sy);
	}
}

static void
default_grab_touch_up(struct weston_touch_grab *grab,
		      uint32_t time, int touch_id)
{
	struct weston_touch *touch = grab->touch;
	struct wl_display *display = touch->seat->compositor->wl_display;
	uint32_t serial;

	if (touch->focus_resource) {
		serial = wl_display_next_serial(display);
		wl_touch_send_up(touch->focus_resource, serial, time, touch_id);
	}
}

static void
default_grab_touch_motion(struct weston_touch_grab *grab, uint32_t time,
			  int touch_id, wl_fixed_t sx, wl_fixed_t sy)
{
	struct weston_touch *touch = grab->touch;

	if (touch->focus_resource) {
		wl_touch_send_motion(touch->focus_resource, time,
				     touch_id, sx, sy);
	}
}

static const struct weston_touch_grab_interface default_touch_grab_interface = {
	default_grab_touch_down,
	default_grab_touch_up,
	default_grab_touch_motion
};

static void
default_grab_key(struct weston_keyboard_grab *grab,
		 uint32_t time, uint32_t key, uint32_t state)
{
	struct weston_keyboard *keyboard = grab->keyboard;
	struct wl_resource *resource;
	struct wl_display *display = keyboard->seat->compositor->wl_display;
	uint32_t serial;

	resource = keyboard->focus_resource;
	if (resource) {
		serial = wl_display_next_serial(display);
		wl_keyboard_send_key(resource, serial, time, key, state);
	}
}

static struct wl_resource *
find_resource_for_surface(struct wl_list *list, struct weston_surface *surface)
{
	if (!surface)
		return NULL;

	if (!surface->resource)
		return NULL;
	
	return wl_resource_find_for_client(list, wl_resource_get_client(surface->resource));
}

static struct wl_resource *
find_resource_for_view(struct wl_list *list, struct weston_view *view)
{
	if (!view)
		return NULL;
	
	return find_resource_for_surface(list, view->surface);
}

static void
default_grab_modifiers(struct weston_keyboard_grab *grab, uint32_t serial,
		       uint32_t mods_depressed, uint32_t mods_latched,
		       uint32_t mods_locked, uint32_t group)
{
	struct weston_keyboard *keyboard = grab->keyboard;
	struct weston_pointer *pointer = keyboard->seat->pointer;
	struct wl_resource *resource, *pr;

	resource = keyboard->focus_resource;
	if (!resource)
		return;

	wl_keyboard_send_modifiers(resource, serial, mods_depressed,
				   mods_latched, mods_locked, group);

	if (pointer && pointer->focus && pointer->focus->surface != keyboard->focus) {
		pr = find_resource_for_view(&keyboard->resource_list,
					    pointer->focus);
		if (pr) {
			wl_keyboard_send_modifiers(pr,
						   serial,
						   keyboard->modifiers.mods_depressed,
						   keyboard->modifiers.mods_latched,
						   keyboard->modifiers.mods_locked,
						   keyboard->modifiers.group);
		}
	}
}

static const struct weston_keyboard_grab_interface
				default_keyboard_grab_interface = {
	default_grab_key,
	default_grab_modifiers,
};

static void
pointer_unmap_sprite(struct weston_pointer *pointer)
{
	if (weston_surface_is_mapped(pointer->sprite->surface))
		weston_surface_unmap(pointer->sprite->surface);

	wl_list_remove(&pointer->sprite_destroy_listener.link);
	pointer->sprite->surface->configure = NULL;
	pointer->sprite->surface->configure_private = NULL;
	weston_view_destroy(pointer->sprite);
	pointer->sprite = NULL;
}

static void
pointer_handle_sprite_destroy(struct wl_listener *listener, void *data)
{
	struct weston_pointer *pointer =
		container_of(listener, struct weston_pointer,
			     sprite_destroy_listener);

	pointer->sprite = NULL;
}

WL_EXPORT struct weston_pointer *
weston_pointer_create(void)
{
	struct weston_pointer *pointer;

	pointer = zalloc(sizeof *pointer);
	if (pointer == NULL)
		return NULL;

	wl_list_init(&pointer->resource_list);
	pointer->focus_listener.notify = lose_pointer_focus;
	pointer->default_grab.interface = &default_pointer_grab_interface;
	pointer->default_grab.pointer = pointer;
	pointer->grab = &pointer->default_grab;
	wl_signal_init(&pointer->focus_signal);

	pointer->sprite_destroy_listener.notify = pointer_handle_sprite_destroy;

	/* FIXME: Pick better co-ords. */
	pointer->x = wl_fixed_from_int(100);
	pointer->y = wl_fixed_from_int(100);

	return pointer;
}

WL_EXPORT void
weston_pointer_destroy(struct weston_pointer *pointer)
{
	if (pointer->sprite)
		pointer_unmap_sprite(pointer);

	/* XXX: What about pointer->resource_list? */
	if (pointer->focus_resource)
		wl_list_remove(&pointer->focus_listener.link);
	free(pointer);
}

WL_EXPORT struct weston_keyboard *
weston_keyboard_create(void)
{
	struct weston_keyboard *keyboard;

	keyboard = zalloc(sizeof *keyboard);
	if (keyboard == NULL)
	    return NULL;

	wl_list_init(&keyboard->resource_list);
	wl_array_init(&keyboard->keys);
	keyboard->focus_listener.notify = lose_keyboard_focus;
	keyboard->default_grab.interface = &default_keyboard_grab_interface;
	keyboard->default_grab.keyboard = keyboard;
	keyboard->grab = &keyboard->default_grab;
	wl_signal_init(&keyboard->focus_signal);

	return keyboard;
}

WL_EXPORT void
weston_keyboard_destroy(struct weston_keyboard *keyboard)
{
	/* XXX: What about keyboard->resource_list? */
	if (keyboard->focus_resource)
		wl_list_remove(&keyboard->focus_listener.link);
	wl_array_release(&keyboard->keys);
	free(keyboard);
}

WL_EXPORT struct weston_touch *
weston_touch_create(void)
{
	struct weston_touch *touch;

	touch = zalloc(sizeof *touch);
	if (touch == NULL)
		return NULL;

	wl_list_init(&touch->resource_list);
	touch->focus_listener.notify = lose_touch_focus;
	touch->default_grab.interface = &default_touch_grab_interface;
	touch->default_grab.touch = touch;
	touch->grab = &touch->default_grab;
	wl_signal_init(&touch->focus_signal);

	return touch;
}

WL_EXPORT void
weston_touch_destroy(struct weston_touch *touch)
{
	/* XXX: What about touch->resource_list? */
	if (touch->focus_resource)
		wl_list_remove(&touch->focus_listener.link);
	free(touch);
}

static void
seat_send_updated_caps(struct weston_seat *seat)
{
	enum wl_seat_capability caps = 0;
	struct wl_resource *resource;

	if (seat->pointer)
		caps |= WL_SEAT_CAPABILITY_POINTER;
	if (seat->keyboard)
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	if (seat->touch)
		caps |= WL_SEAT_CAPABILITY_TOUCH;

	wl_resource_for_each(resource, &seat->base_resource_list) {
		wl_seat_send_capabilities(resource, caps);
	}
}

WL_EXPORT void
weston_pointer_set_focus(struct weston_pointer *pointer,
			 struct weston_view *view,
			 wl_fixed_t sx, wl_fixed_t sy)
{
	struct weston_keyboard *kbd = pointer->seat->keyboard;
	struct wl_resource *resource, *kr;
	struct wl_display *display = pointer->seat->compositor->wl_display;
	uint32_t serial;

	resource = pointer->focus_resource;
	if ((resource && !view) ||
	    (resource && view && pointer->focus->surface != view->surface)) {
		display = wl_client_get_display(wl_resource_get_client(resource));
		serial = wl_display_next_serial(display);
		wl_pointer_send_leave(resource, serial,
				      pointer->focus->surface->resource);
		wl_list_remove(&pointer->focus_listener.link);
	}

	resource = find_resource_for_view(&pointer->resource_list,
					  view);
	if (resource &&
	    (!pointer->focus ||
	     pointer->focus->surface != view->surface ||
	     pointer->focus_resource != resource)) {
		serial = wl_display_next_serial(display);
		if (kbd) {
			kr = find_resource_for_view(&kbd->resource_list,
						    view);
			if (kr) {
				wl_keyboard_send_modifiers(kr,
							   serial,
							   kbd->modifiers.mods_depressed,
							   kbd->modifiers.mods_latched,
							   kbd->modifiers.mods_locked,
							   kbd->modifiers.group);
			}
		}
		wl_pointer_send_enter(resource, serial, view->surface->resource,
				      sx, sy);
		wl_resource_add_destroy_listener(resource,
						 &pointer->focus_listener);
		pointer->focus_serial = serial;
	}

	pointer->focus_resource = resource;
	pointer->focus = view;
	wl_signal_emit(&pointer->focus_signal, pointer);
}

WL_EXPORT void
weston_keyboard_set_focus(struct weston_keyboard *keyboard,
			  struct weston_surface *surface)
{
	struct wl_resource *resource;
	struct wl_display *display = keyboard->seat->compositor->wl_display;
	uint32_t serial;

	if (keyboard->focus_resource && keyboard->focus != surface) {
		resource = keyboard->focus_resource;
		serial = wl_display_next_serial(display);
		wl_keyboard_send_leave(resource, serial,
				       keyboard->focus->resource);
		wl_list_remove(&keyboard->focus_listener.link);
	}

	resource = find_resource_for_surface(&keyboard->resource_list,
					     surface);
	if (resource &&
	    (keyboard->focus != surface ||
	     keyboard->focus_resource != resource)) {
		serial = wl_display_next_serial(display);
		wl_keyboard_send_modifiers(resource, serial,
					   keyboard->modifiers.mods_depressed,
					   keyboard->modifiers.mods_latched,
					   keyboard->modifiers.mods_locked,
					   keyboard->modifiers.group);
		wl_keyboard_send_enter(resource, serial, surface->resource,
				       &keyboard->keys);
		wl_resource_add_destroy_listener(resource,
						 &keyboard->focus_listener);
		keyboard->focus_serial = serial;
	}

	keyboard->focus_resource = resource;
	keyboard->focus = surface;
	wl_signal_emit(&keyboard->focus_signal, keyboard);
}

WL_EXPORT void
weston_keyboard_start_grab(struct weston_keyboard *keyboard,
			   struct weston_keyboard_grab *grab)
{
	keyboard->grab = grab;
	grab->keyboard = keyboard;

	/* XXX focus? */
}

WL_EXPORT void
weston_keyboard_end_grab(struct weston_keyboard *keyboard)
{
	keyboard->grab = &keyboard->default_grab;
}

WL_EXPORT void
weston_pointer_start_grab(struct weston_pointer *pointer,
			  struct weston_pointer_grab *grab)
{
	pointer->grab = grab;
	grab->pointer = pointer;
	pointer->grab->interface->focus(pointer->grab);
}

WL_EXPORT void
weston_pointer_end_grab(struct weston_pointer *pointer)
{
	pointer->grab = &pointer->default_grab;
	pointer->grab->interface->focus(pointer->grab);
}

WL_EXPORT void
weston_touch_start_grab(struct weston_touch *touch, struct weston_touch_grab *grab)
{
	touch->grab = grab;
	grab->touch = touch;
}

WL_EXPORT void
weston_touch_end_grab(struct weston_touch *touch)
{
	touch->grab = &touch->default_grab;
}

WL_EXPORT void
weston_pointer_clamp(struct weston_pointer *pointer, wl_fixed_t *fx, wl_fixed_t *fy)
{
	struct weston_compositor *ec = pointer->seat->compositor;
	struct weston_output *output, *prev = NULL;
	int x, y, old_x, old_y, valid = 0;

	x = wl_fixed_to_int(*fx);
	y = wl_fixed_to_int(*fy);
	old_x = wl_fixed_to_int(pointer->x);
	old_y = wl_fixed_to_int(pointer->y);

	wl_list_for_each(output, &ec->output_list, link) {
		if (pointer->seat->output && pointer->seat->output != output)
			continue;
		if (pixman_region32_contains_point(&output->region,
						   x, y, NULL))
			valid = 1;
		if (pixman_region32_contains_point(&output->region,
						   old_x, old_y, NULL))
			prev = output;
	}

	if (!prev)
		prev = pointer->seat->output;

	if (prev && !valid) {
		if (x < prev->x)
			*fx = wl_fixed_from_int(prev->x);
		else if (x >= prev->x + prev->width)
			*fx = wl_fixed_from_int(prev->x +
						prev->width - 1);
		if (y < prev->y)
			*fy = wl_fixed_from_int(prev->y);
		else if (y >= prev->y + prev->height)
			*fy = wl_fixed_from_int(prev->y +
						prev->height - 1);
	}
}

/* Takes absolute values */
static void
move_pointer(struct weston_seat *seat, wl_fixed_t x, wl_fixed_t y)
{
	struct weston_compositor *ec = seat->compositor;
	struct weston_pointer *pointer = seat->pointer;
	struct weston_output *output;
	int32_t ix, iy;

	weston_pointer_clamp (pointer, &x, &y);

	pointer->x = x;
	pointer->y = y;

	ix = wl_fixed_to_int(x);
	iy = wl_fixed_to_int(y);

	wl_list_for_each(output, &ec->output_list, link)
		if (output->zoom.active &&
		    pixman_region32_contains_point(&output->region,
						   ix, iy, NULL))
			weston_output_update_zoom(output);

	if (pointer->sprite) {
		weston_view_set_position(pointer->sprite,
					 ix - pointer->hotspot_x,
					 iy - pointer->hotspot_y);
		weston_view_schedule_repaint(pointer->sprite);
	}
}

WL_EXPORT void
notify_motion(struct weston_seat *seat,
	      uint32_t time, wl_fixed_t dx, wl_fixed_t dy)
{
	struct weston_compositor *ec = seat->compositor;
	struct weston_pointer *pointer = seat->pointer;

	weston_compositor_wake(ec);

	move_pointer(seat, pointer->x + dx, pointer->y + dy);

	pointer->grab->interface->focus(pointer->grab);
	pointer->grab->interface->motion(pointer->grab, time);
}

WL_EXPORT void
notify_motion_absolute(struct weston_seat *seat,
		       uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	struct weston_compositor *ec = seat->compositor;
	struct weston_pointer *pointer = seat->pointer;

	weston_compositor_wake(ec);

	move_pointer(seat, x, y);

	pointer->grab->interface->focus(pointer->grab);
	pointer->grab->interface->motion(pointer->grab, time);
}

WL_EXPORT void
weston_surface_activate(struct weston_surface *surface,
			struct weston_seat *seat)
{
	struct weston_compositor *compositor = seat->compositor;

	if (seat->keyboard) {
		weston_keyboard_set_focus(seat->keyboard, surface);
		wl_data_device_set_keyboard_focus(seat);
	}

	wl_signal_emit(&compositor->activate_signal, surface);
}

WL_EXPORT void
notify_button(struct weston_seat *seat, uint32_t time, int32_t button,
	      enum wl_pointer_button_state state)
{
	struct weston_compositor *compositor = seat->compositor;
	struct weston_pointer *pointer = seat->pointer;
	struct weston_surface *focus =
		(struct weston_surface *) pointer->focus;
	uint32_t serial = wl_display_next_serial(compositor->wl_display);

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (compositor->ping_handler && focus)
			compositor->ping_handler(focus, serial);
		weston_compositor_idle_inhibit(compositor);
		if (pointer->button_count == 0) {
			pointer->grab_button = button;
			pointer->grab_time = time;
			pointer->grab_x = pointer->x;
			pointer->grab_y = pointer->y;
		}
		pointer->button_count++;
	} else {
		weston_compositor_idle_release(compositor);
		pointer->button_count--;
	}

	weston_compositor_run_button_binding(compositor, seat, time, button,
					     state);

	pointer->grab->interface->button(pointer->grab, time, button, state);

	if (pointer->button_count == 1)
		pointer->grab_serial =
			wl_display_get_serial(compositor->wl_display);
}

WL_EXPORT void
notify_axis(struct weston_seat *seat, uint32_t time, uint32_t axis,
	    wl_fixed_t value)
{
	struct weston_compositor *compositor = seat->compositor;
	struct weston_pointer *pointer = seat->pointer;
	struct weston_surface *focus =
		(struct weston_surface *) pointer->focus;
	uint32_t serial = wl_display_next_serial(compositor->wl_display);

	if (compositor->ping_handler && focus)
		compositor->ping_handler(focus, serial);

	weston_compositor_wake(compositor);

	if (!value)
		return;

	if (weston_compositor_run_axis_binding(compositor, seat,
						   time, axis, value))
		return;

	if (pointer->focus_resource)
		wl_pointer_send_axis(pointer->focus_resource, time, axis,
				     value);
}

#ifdef ENABLE_XKBCOMMON
WL_EXPORT void
notify_modifiers(struct weston_seat *seat, uint32_t serial)
{
	struct weston_keyboard *keyboard = seat->keyboard;
	struct weston_keyboard_grab *grab = keyboard->grab;
	uint32_t mods_depressed, mods_latched, mods_locked, group;
	uint32_t mods_lookup;
	enum weston_led leds = 0;
	int changed = 0;

	/* Serialize and update our internal state, checking to see if it's
	 * different to the previous state. */
	mods_depressed = xkb_state_serialize_mods(seat->xkb_state.state,
						  XKB_STATE_DEPRESSED);
	mods_latched = xkb_state_serialize_mods(seat->xkb_state.state,
						XKB_STATE_LATCHED);
	mods_locked = xkb_state_serialize_mods(seat->xkb_state.state,
					       XKB_STATE_LOCKED);
	group = xkb_state_serialize_group(seat->xkb_state.state,
					  XKB_STATE_EFFECTIVE);

	if (mods_depressed != seat->keyboard->modifiers.mods_depressed ||
	    mods_latched != seat->keyboard->modifiers.mods_latched ||
	    mods_locked != seat->keyboard->modifiers.mods_locked ||
	    group != seat->keyboard->modifiers.group)
		changed = 1;

	seat->keyboard->modifiers.mods_depressed = mods_depressed;
	seat->keyboard->modifiers.mods_latched = mods_latched;
	seat->keyboard->modifiers.mods_locked = mods_locked;
	seat->keyboard->modifiers.group = group;

	/* And update the modifier_state for bindings. */
	mods_lookup = mods_depressed | mods_latched;
	seat->modifier_state = 0;
	if (mods_lookup & (1 << seat->xkb_info->ctrl_mod))
		seat->modifier_state |= MODIFIER_CTRL;
	if (mods_lookup & (1 << seat->xkb_info->alt_mod))
		seat->modifier_state |= MODIFIER_ALT;
	if (mods_lookup & (1 << seat->xkb_info->super_mod))
		seat->modifier_state |= MODIFIER_SUPER;
	if (mods_lookup & (1 << seat->xkb_info->shift_mod))
		seat->modifier_state |= MODIFIER_SHIFT;

	/* Finally, notify the compositor that LEDs have changed. */
	if (xkb_state_led_index_is_active(seat->xkb_state.state,
					  seat->xkb_info->num_led))
		leds |= LED_NUM_LOCK;
	if (xkb_state_led_index_is_active(seat->xkb_state.state,
					  seat->xkb_info->caps_led))
		leds |= LED_CAPS_LOCK;
	if (xkb_state_led_index_is_active(seat->xkb_state.state,
					  seat->xkb_info->scroll_led))
		leds |= LED_SCROLL_LOCK;
	if (leds != seat->xkb_state.leds && seat->led_update)
		seat->led_update(seat, leds);
	seat->xkb_state.leds = leds;

	if (changed) {
		grab->interface->modifiers(grab,
					   serial,
					   keyboard->modifiers.mods_depressed,
					   keyboard->modifiers.mods_latched,
					   keyboard->modifiers.mods_locked,
					   keyboard->modifiers.group);
	}
}

static void
update_modifier_state(struct weston_seat *seat, uint32_t serial, uint32_t key,
		      enum wl_keyboard_key_state state)
{
	enum xkb_key_direction direction;

	/* Keyboard modifiers don't exist in raw keyboard mode */
	if (!seat->compositor->use_xkbcommon)
		return;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		direction = XKB_KEY_DOWN;
	else
		direction = XKB_KEY_UP;

	/* Offset the keycode by 8, as the evdev XKB rules reflect X's
	 * broken keycode system, which starts at 8. */
	xkb_state_update_key(seat->xkb_state.state, key + 8, direction);

	notify_modifiers(seat, serial);
}
#else
WL_EXPORT void
notify_modifiers(struct weston_seat *seat, uint32_t serial)
{
}

static void
update_modifier_state(struct weston_seat *seat, uint32_t serial, uint32_t key,
		      enum wl_keyboard_key_state state)
{
}
#endif

WL_EXPORT void
notify_key(struct weston_seat *seat, uint32_t time, uint32_t key,
	   enum wl_keyboard_key_state state,
	   enum weston_key_state_update update_state)
{
	struct weston_compositor *compositor = seat->compositor;
	struct weston_keyboard *keyboard = seat->keyboard;
	struct weston_surface *focus =
		(struct weston_surface *) keyboard->focus;
	struct weston_keyboard_grab *grab = keyboard->grab;
	uint32_t serial = wl_display_next_serial(compositor->wl_display);
	uint32_t *k, *end;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (compositor->ping_handler && focus)
			compositor->ping_handler(focus, serial);

		weston_compositor_idle_inhibit(compositor);
		keyboard->grab_key = key;
		keyboard->grab_time = time;
	} else {
		weston_compositor_idle_release(compositor);
	}

	end = keyboard->keys.data + keyboard->keys.size;
	for (k = keyboard->keys.data; k < end; k++) {
		if (*k == key) {
			/* Ignore server-generated repeats. */
			if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
				return;
			*k = *--end;
		}
	}
	keyboard->keys.size = (void *) end - keyboard->keys.data;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		k = wl_array_add(&keyboard->keys, sizeof *k);
		*k = key;
	}

	if (grab == &keyboard->default_grab ||
	    grab == &keyboard->input_method_grab) {
		weston_compositor_run_key_binding(compositor, seat, time, key,
						  state);
		grab = keyboard->grab;
	}

	grab->interface->key(grab, time, key, state);

	if (update_state == STATE_UPDATE_AUTOMATIC) {
		update_modifier_state(seat,
				      wl_display_get_serial(compositor->wl_display),
				      key,
				      state);
	}
}

WL_EXPORT void
notify_pointer_focus(struct weston_seat *seat, struct weston_output *output,
		     wl_fixed_t x, wl_fixed_t y)
{
	struct weston_compositor *compositor = seat->compositor;

	if (output) {
		move_pointer(seat, x, y);
		compositor->focus = 1;
	} else {
		compositor->focus = 0;
		/* FIXME: We should call weston_pointer_set_focus(seat,
		 * NULL) here, but somehow that breaks re-entry... */
	}
}

static void
destroy_device_saved_kbd_focus(struct wl_listener *listener, void *data)
{
	struct weston_seat *ws;

	ws = container_of(listener, struct weston_seat,
			  saved_kbd_focus_listener);

	ws->saved_kbd_focus = NULL;
}

WL_EXPORT void
notify_keyboard_focus_in(struct weston_seat *seat, struct wl_array *keys,
			 enum weston_key_state_update update_state)
{
	struct weston_compositor *compositor = seat->compositor;
	struct weston_keyboard *keyboard = seat->keyboard;
	struct weston_surface *surface;
	uint32_t *k, serial;

	serial = wl_display_next_serial(compositor->wl_display);
	wl_array_copy(&keyboard->keys, keys);
	wl_array_for_each(k, &keyboard->keys) {
		weston_compositor_idle_inhibit(compositor);
		if (update_state == STATE_UPDATE_AUTOMATIC)
			update_modifier_state(seat, serial, *k,
					      WL_KEYBOARD_KEY_STATE_PRESSED);
	}

	/* Run key bindings after we've updated the state. */
	wl_array_for_each(k, &keyboard->keys) {
		weston_compositor_run_key_binding(compositor, seat, 0, *k,
						  WL_KEYBOARD_KEY_STATE_PRESSED);
	}

	surface = seat->saved_kbd_focus;

	if (surface) {
		wl_list_remove(&seat->saved_kbd_focus_listener.link);
		weston_keyboard_set_focus(keyboard, surface);
		seat->saved_kbd_focus = NULL;
	}
}

WL_EXPORT void
notify_keyboard_focus_out(struct weston_seat *seat)
{
	struct weston_compositor *compositor = seat->compositor;
	struct weston_keyboard *keyboard = seat->keyboard;
	uint32_t *k, serial;

	serial = wl_display_next_serial(compositor->wl_display);
	wl_array_for_each(k, &keyboard->keys) {
		weston_compositor_idle_release(compositor);
		update_modifier_state(seat, serial, *k,
				      WL_KEYBOARD_KEY_STATE_RELEASED);
	}

	seat->modifier_state = 0;

	if (keyboard->focus) {
		seat->saved_kbd_focus = keyboard->focus;
		seat->saved_kbd_focus_listener.notify =
			destroy_device_saved_kbd_focus;
		wl_signal_add(&keyboard->focus->destroy_signal,
			      &seat->saved_kbd_focus_listener);
	}

	weston_keyboard_set_focus(keyboard, NULL);
	/* FIXME: We really need keyboard grab cancel here to
	 * let the grab shut down properly.  As it is we leak
	 * the grab data. */
	weston_keyboard_end_grab(keyboard);
}

WL_EXPORT void
weston_touch_set_focus(struct weston_seat *seat, struct weston_view *view)
{
	struct wl_resource *resource;

	if (seat->touch->focus->surface == view->surface) {
		seat->touch->focus = view;
		return;
	}

	if (seat->touch->focus_resource)
		wl_list_remove(&seat->touch->focus_listener.link);
	seat->touch->focus = NULL;
	seat->touch->focus_resource = NULL;

	if (view) {
		resource =
			find_resource_for_surface(&seat->touch->resource_list,
						  view->surface);
		if (!resource) {
			weston_log("couldn't find resource\n");
			return;
		}

		seat->touch->focus = view;
		seat->touch->focus_resource = resource;
		wl_resource_add_destroy_listener(resource,
						 &seat->touch->focus_listener);
	}
}

/**
 * notify_touch - emulates button touches and notifies surfaces accordingly.
 *
 * It assumes always the correct cycle sequence until it gets here: touch_down
 * → touch_update → ... → touch_update → touch_end. The driver is responsible
 * for sending along such order.
 *
 */
WL_EXPORT void
notify_touch(struct weston_seat *seat, uint32_t time, int touch_id,
             wl_fixed_t x, wl_fixed_t y, int touch_type)
{
	struct weston_compositor *ec = seat->compositor;
	struct weston_touch *touch = seat->touch;
	struct weston_touch_grab *grab = touch->grab;
	struct weston_view *ev;
	wl_fixed_t sx, sy;

	/* Update grab's global coordinates. */
	touch->grab_x = x;
	touch->grab_y = y;

	switch (touch_type) {
	case WL_TOUCH_DOWN:
		weston_compositor_idle_inhibit(ec);

		seat->num_tp++;

		/* the first finger down picks the view, and all further go
		 * to that view for the remainder of the touch session i.e.
		 * until all touch points are up again. */
		if (seat->num_tp == 1) {
			ev = weston_compositor_pick_view(ec, x, y, &sx, &sy);
			weston_touch_set_focus(seat, ev);
		} else if (touch->focus) {
			ev = touch->focus;
			weston_view_from_global_fixed(ev, x, y, &sx, &sy);
		} else {
			/* Unexpected condition: We have non-initial touch but
			 * there is no focused surface.
			 */
			weston_log("touch event received with %d points down"
				   "but no surface focused\n", seat->num_tp);
			return;
		}

		grab->interface->down(grab, time, touch_id, sx, sy);
		if (seat->num_tp == 1) {
			touch->grab_serial =
				wl_display_get_serial(ec->wl_display);
			touch->grab_time = time;
			touch->grab_x = x;
			touch->grab_y = y;
		}

		break;
	case WL_TOUCH_MOTION:
		ev = touch->focus;
		if (!ev)
			break;

		weston_view_from_global_fixed(ev, x, y, &sx, &sy);
		grab->interface->motion(grab, time, touch_id, sx, sy);
		break;
	case WL_TOUCH_UP:
		weston_compositor_idle_release(ec);
		seat->num_tp--;

		grab->interface->up(grab, time, touch_id);
		if (seat->num_tp == 0)
			weston_touch_set_focus(seat, NULL);
		break;
	}
}

static void
pointer_cursor_surface_configure(struct weston_surface *es,
				 int32_t dx, int32_t dy, int32_t width, int32_t height)
{
	struct weston_pointer *pointer = es->configure_private;
	int x, y;

	if (width == 0)
		return;

	assert(es == pointer->sprite->surface);

	pointer->hotspot_x -= dx;
	pointer->hotspot_y -= dy;

	x = wl_fixed_to_int(pointer->x) - pointer->hotspot_x;
	y = wl_fixed_to_int(pointer->y) - pointer->hotspot_y;

	weston_view_configure(pointer->sprite, x, y, width, height);

	empty_region(&es->pending.input);

	if (!weston_surface_is_mapped(es)) {
		wl_list_insert(&es->compositor->cursor_layer.view_list,
			       &pointer->sprite->layer_link);
		weston_view_update_transform(pointer->sprite);
	}
}

static void
pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
		   uint32_t serial, struct wl_resource *surface_resource,
		   int32_t x, int32_t y)
{
	struct weston_pointer *pointer = wl_resource_get_user_data(resource);
	struct weston_surface *surface = NULL;

	if (surface_resource)
		surface = wl_resource_get_user_data(surface_resource);

	if (pointer->focus == NULL)
		return;
	/* pointer->focus->surface->resource can be NULL. Surfaces like the
	black_surface used in shell.c for fullscreen don't have
	a resource, but can still have focus */
	if (pointer->focus->surface->resource == NULL)
		return;
	if (wl_resource_get_client(pointer->focus->surface->resource) != client)
		return;
	if (pointer->focus_serial - serial > UINT32_MAX / 2)
		return;

	if (surface && pointer->sprite && surface != pointer->sprite->surface) {
		if (surface->configure) {
			wl_resource_post_error(surface->resource,
					       WL_DISPLAY_ERROR_INVALID_OBJECT,
					       "surface->configure already "
					       "set");
			return;
		}
	}

	if (pointer->sprite)
		pointer_unmap_sprite(pointer);

	if (!surface)
		return;

	wl_signal_add(&surface->destroy_signal,
		      &pointer->sprite_destroy_listener);

	surface->configure = pointer_cursor_surface_configure;
	surface->configure_private = pointer;
	pointer->sprite = weston_view_create(surface);
	pointer->hotspot_x = x;
	pointer->hotspot_y = y;

	if (surface->buffer_ref.buffer)
		pointer_cursor_surface_configure(surface, 0, 0, weston_surface_buffer_width(surface),
								weston_surface_buffer_height(surface));
}

static void
pointer_release(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_interface = {
	pointer_set_cursor,
	pointer_release
};

static void
seat_get_pointer(struct wl_client *client, struct wl_resource *resource,
		 uint32_t id)
{
	struct weston_seat *seat = wl_resource_get_user_data(resource);
	struct wl_resource *cr;

	if (!seat->pointer)
		return;

        cr = wl_resource_create(client, &wl_pointer_interface,
				wl_resource_get_version(resource), id);
	if (cr == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_list_insert(&seat->pointer->resource_list, wl_resource_get_link(cr));
	wl_resource_set_implementation(cr, &pointer_interface, seat->pointer,
				       unbind_resource);

	if (seat->pointer->focus && seat->pointer->focus->surface->resource &&
	    wl_resource_get_client(seat->pointer->focus->surface->resource) == client) {
		wl_fixed_t sx, sy;

		weston_view_from_global_fixed(seat->pointer->focus,
					      seat->pointer->x,
					      seat->pointer->y,
					      &sx, &sy);
		weston_pointer_set_focus(seat->pointer,
					 seat->pointer->focus,
					 sx, sy);
	}
}

static void
keyboard_release(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_interface = {
	keyboard_release
};

static void
seat_get_keyboard(struct wl_client *client, struct wl_resource *resource,
		  uint32_t id)
{
	struct weston_seat *seat = wl_resource_get_user_data(resource);
	struct wl_resource *cr;

	if (!seat->keyboard)
		return;

        cr = wl_resource_create(client, &wl_keyboard_interface,
				wl_resource_get_version(resource), id);
	if (cr == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_list_insert(&seat->keyboard->resource_list, wl_resource_get_link(cr));
	wl_resource_set_implementation(cr, &keyboard_interface,
				       seat, unbind_resource);

	if (seat->compositor->use_xkbcommon) {
		wl_keyboard_send_keymap(cr, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
					seat->xkb_info->keymap_fd,
					seat->xkb_info->keymap_size);
	} else {
		int null_fd = open("/dev/null", O_RDONLY);
		wl_keyboard_send_keymap(cr, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP,
					null_fd,
					0);
		close(null_fd);
	}

	if (seat->keyboard->focus &&
	    wl_resource_get_client(seat->keyboard->focus->resource) == client) {
		weston_keyboard_set_focus(seat->keyboard,
					  seat->keyboard->focus);
		wl_data_device_set_keyboard_focus(seat);
	}
}

static void
touch_release(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_touch_interface touch_interface = {
	touch_release
};

static void
seat_get_touch(struct wl_client *client, struct wl_resource *resource,
	       uint32_t id)
{
	struct weston_seat *seat = wl_resource_get_user_data(resource);
	struct wl_resource *cr;

	if (!seat->touch)
		return;

        cr = wl_resource_create(client, &wl_touch_interface,
				wl_resource_get_version(resource), id);
	if (cr == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_list_insert(&seat->touch->resource_list, wl_resource_get_link(cr));
	wl_resource_set_implementation(cr, &touch_interface,
				       seat, unbind_resource);
}

static const struct wl_seat_interface seat_interface = {
	seat_get_pointer,
	seat_get_keyboard,
	seat_get_touch,
};

static void
bind_seat(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct weston_seat *seat = data;
	struct wl_resource *resource;
	enum wl_seat_capability caps = 0;

	resource = wl_resource_create(client,
				      &wl_seat_interface, MIN(version, 3), id);
	wl_list_insert(&seat->base_resource_list, wl_resource_get_link(resource));
	wl_resource_set_implementation(resource, &seat_interface, data,
				       unbind_resource);

	if (seat->pointer)
		caps |= WL_SEAT_CAPABILITY_POINTER;
	if (seat->keyboard)
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	if (seat->touch)
		caps |= WL_SEAT_CAPABILITY_TOUCH;

	wl_seat_send_capabilities(resource, caps);
	if (version >= 2)
		wl_seat_send_name(resource, seat->seat_name);
}

#ifdef ENABLE_XKBCOMMON
int
weston_compositor_xkb_init(struct weston_compositor *ec,
			   struct xkb_rule_names *names)
{
	ec->use_xkbcommon = 1;

	if (ec->xkb_context == NULL) {
		ec->xkb_context = xkb_context_new(0);
		if (ec->xkb_context == NULL) {
			weston_log("failed to create XKB context\n");
			return -1;
		}
	}

	if (names)
		ec->xkb_names = *names;
	if (!ec->xkb_names.rules)
		ec->xkb_names.rules = strdup("evdev");
	if (!ec->xkb_names.model)
		ec->xkb_names.model = strdup("pc105");
	if (!ec->xkb_names.layout)
		ec->xkb_names.layout = strdup("us");

	return 0;
}

static void 
weston_xkb_info_destroy(struct weston_xkb_info *xkb_info)
{
	if (--xkb_info->ref_count > 0)
		return;

	if (xkb_info->keymap)
		xkb_map_unref(xkb_info->keymap);

	if (xkb_info->keymap_area)
		munmap(xkb_info->keymap_area, xkb_info->keymap_size);
	if (xkb_info->keymap_fd >= 0)
		close(xkb_info->keymap_fd);
	free(xkb_info);
}

void
weston_compositor_xkb_destroy(struct weston_compositor *ec)
{
	/*
	 * If we're operating in raw keyboard mode, we never initialized
	 * libxkbcommon so there's no cleanup to do either.
	 */
	if (!ec->use_xkbcommon)
		return;

	free((char *) ec->xkb_names.rules);
	free((char *) ec->xkb_names.model);
	free((char *) ec->xkb_names.layout);
	free((char *) ec->xkb_names.variant);
	free((char *) ec->xkb_names.options);
	
	if (ec->xkb_info)
		weston_xkb_info_destroy(ec->xkb_info);
	xkb_context_unref(ec->xkb_context);
}

static struct weston_xkb_info *
weston_xkb_info_create(struct xkb_keymap *keymap)
{
	struct weston_xkb_info *xkb_info = zalloc(sizeof *xkb_info);
	if (xkb_info == NULL)
		return NULL;

	xkb_info->keymap = xkb_map_ref(keymap);
	xkb_info->ref_count = 1;

	char *keymap_str;

	xkb_info->shift_mod = xkb_map_mod_get_index(xkb_info->keymap,
						    XKB_MOD_NAME_SHIFT);
	xkb_info->caps_mod = xkb_map_mod_get_index(xkb_info->keymap,
						   XKB_MOD_NAME_CAPS);
	xkb_info->ctrl_mod = xkb_map_mod_get_index(xkb_info->keymap,
						   XKB_MOD_NAME_CTRL);
	xkb_info->alt_mod = xkb_map_mod_get_index(xkb_info->keymap,
						  XKB_MOD_NAME_ALT);
	xkb_info->mod2_mod = xkb_map_mod_get_index(xkb_info->keymap, "Mod2");
	xkb_info->mod3_mod = xkb_map_mod_get_index(xkb_info->keymap, "Mod3");
	xkb_info->super_mod = xkb_map_mod_get_index(xkb_info->keymap,
						    XKB_MOD_NAME_LOGO);
	xkb_info->mod5_mod = xkb_map_mod_get_index(xkb_info->keymap, "Mod5");

	xkb_info->num_led = xkb_map_led_get_index(xkb_info->keymap,
						  XKB_LED_NAME_NUM);
	xkb_info->caps_led = xkb_map_led_get_index(xkb_info->keymap,
						   XKB_LED_NAME_CAPS);
	xkb_info->scroll_led = xkb_map_led_get_index(xkb_info->keymap,
						     XKB_LED_NAME_SCROLL);

	keymap_str = xkb_map_get_as_string(xkb_info->keymap);
	if (keymap_str == NULL) {
		weston_log("failed to get string version of keymap\n");
		goto err_keymap;
	}
	xkb_info->keymap_size = strlen(keymap_str) + 1;

	xkb_info->keymap_fd = os_create_anonymous_file(xkb_info->keymap_size);
	if (xkb_info->keymap_fd < 0) {
		weston_log("creating a keymap file for %lu bytes failed: %m\n",
			(unsigned long) xkb_info->keymap_size);
		goto err_keymap_str;
	}

	xkb_info->keymap_area = mmap(NULL, xkb_info->keymap_size,
				     PROT_READ | PROT_WRITE,
				     MAP_SHARED, xkb_info->keymap_fd, 0);
	if (xkb_info->keymap_area == MAP_FAILED) {
		weston_log("failed to mmap() %lu bytes\n",
			(unsigned long) xkb_info->keymap_size);
		goto err_dev_zero;
	}
	strcpy(xkb_info->keymap_area, keymap_str);
	free(keymap_str);

	return xkb_info;

err_dev_zero:
	close(xkb_info->keymap_fd);
err_keymap_str:
	free(keymap_str);
err_keymap:
	xkb_map_unref(xkb_info->keymap);
	free(xkb_info);
	return NULL;
}

static int
weston_compositor_build_global_keymap(struct weston_compositor *ec)
{
	struct xkb_keymap *keymap;

	if (ec->xkb_info != NULL)
		return 0;

	keymap = xkb_map_new_from_names(ec->xkb_context,
					&ec->xkb_names,
					0);
	if (keymap == NULL) {
		weston_log("failed to compile global XKB keymap\n");
		weston_log("  tried rules %s, model %s, layout %s, variant %s, "
			"options %s\n",
			ec->xkb_names.rules, ec->xkb_names.model,
			ec->xkb_names.layout, ec->xkb_names.variant,
			ec->xkb_names.options);
		return -1;
	}

	ec->xkb_info = weston_xkb_info_create(keymap);
	if (ec->xkb_info == NULL) 
		return -1;

	return 0;
}
#else
int
weston_compositor_xkb_init(struct weston_compositor *ec,
			   struct xkb_rule_names *names)
{
	return 0;
}

void
weston_compositor_xkb_destroy(struct weston_compositor *ec)
{
}
#endif

WL_EXPORT int
weston_seat_init_keyboard(struct weston_seat *seat, struct xkb_keymap *keymap)
{
	struct weston_keyboard *keyboard;

	if (seat->keyboard)
		return 0;

#ifdef ENABLE_XKBCOMMON
	if (seat->compositor->use_xkbcommon) {
		if (keymap != NULL) {
			seat->xkb_info = weston_xkb_info_create(keymap);
			if (seat->xkb_info == NULL)
				return -1;
		} else {
			if (weston_compositor_build_global_keymap(seat->compositor) < 0)
				return -1;
			seat->xkb_info = seat->compositor->xkb_info;
			seat->xkb_info->ref_count++;
		}

		seat->xkb_state.state = xkb_state_new(seat->xkb_info->keymap);
		if (seat->xkb_state.state == NULL) {
			weston_log("failed to initialise XKB state\n");
			return -1;
		}

		seat->xkb_state.leds = 0;
	}
#endif

	keyboard = weston_keyboard_create();
	if (keyboard == NULL) {
		weston_log("failed to allocate weston keyboard struct\n");
		return -1;
	}

	seat->keyboard = keyboard;
	keyboard->seat = seat;

	seat_send_updated_caps(seat);

	return 0;
}

WL_EXPORT void
weston_seat_init_pointer(struct weston_seat *seat)
{
	struct weston_pointer *pointer;

	if (seat->pointer)
		return;

	pointer = weston_pointer_create();
	if (pointer == NULL)
		return;

	seat->pointer = pointer;
		pointer->seat = seat;

	seat_send_updated_caps(seat);
}

WL_EXPORT void
weston_seat_init_touch(struct weston_seat *seat)
{
	struct weston_touch *touch;

	if (seat->touch)
		return;

	touch = weston_touch_create();
	if (touch == NULL)
		return;

	seat->touch = touch;
	touch->seat = seat;

	seat_send_updated_caps(seat);
}

WL_EXPORT void
weston_seat_init(struct weston_seat *seat, struct weston_compositor *ec,
		 const char *seat_name)
{
	memset(seat, 0, sizeof *seat);

	seat->selection_data_source = NULL;
	wl_list_init(&seat->base_resource_list);
	wl_signal_init(&seat->selection_signal);
	wl_list_init(&seat->drag_resource_list);
	wl_signal_init(&seat->destroy_signal);

	seat->global = wl_global_create(ec->wl_display, &wl_seat_interface, 3,
					seat, bind_seat);

	seat->compositor = ec;
	seat->modifier_state = 0;
	seat->num_tp = 0;
	seat->seat_name = strdup(seat_name);

	wl_list_insert(ec->seat_list.prev, &seat->link);

	clipboard_create(seat);

	wl_signal_emit(&ec->seat_created_signal, seat);
}

WL_EXPORT void
weston_seat_release(struct weston_seat *seat)
{
	wl_list_remove(&seat->link);

#ifdef ENABLE_XKBCOMMON
	if (seat->compositor->use_xkbcommon) {
		if (seat->xkb_state.state != NULL)
			xkb_state_unref(seat->xkb_state.state);
		if (seat->xkb_info)
			weston_xkb_info_destroy(seat->xkb_info);
	}
#endif

	if (seat->pointer)
		weston_pointer_destroy(seat->pointer);
	if (seat->keyboard)
		weston_keyboard_destroy(seat->keyboard);
	if (seat->touch)
		weston_touch_destroy(seat->touch);

	free (seat->seat_name);

	wl_global_destroy(seat->global);

	wl_signal_emit(&seat->destroy_signal, seat);
}
