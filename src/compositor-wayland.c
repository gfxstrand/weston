/*
 * Copyright © 2010-2011 Benjamin Franzke
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

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>

#include "compositor.h"
#include "gl-renderer.h"
#include "../shared/image-loader.h"
#include "../shared/os-compatibility.h"
#include "../shared/cairo-util.h"

struct wayland_compositor {
	struct weston_compositor base;

	struct {
		struct wl_display *wl_display;
		struct wl_registry *registry;
		struct wl_compositor *compositor;
		struct wl_shell *shell;
		struct wl_output *output;
		struct wl_shm *shm;

		struct {
			int32_t x, y, width, height;
		} screen_allocation;

		struct wl_event_source *wl_source;
		uint32_t event_mask;
	} parent;

	struct theme *theme;
	cairo_device_t *frame_device;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor *cursor;

	struct wl_list inputs;
};

struct wayland_output {
	struct weston_output base;

	struct {
		int draw_initial_frame;
		struct wl_surface *surface;
		struct wl_shell_surface *shell_surface;
		struct wl_egl_window *egl_window;
	} parent;

	int keyboard_count;

	struct frame *frame;
	struct {
		cairo_surface_t *top;
		cairo_surface_t *left;
		cairo_surface_t *right;
		cairo_surface_t *bottom;
	} border;

	struct weston_mode mode;
};

struct wayland_input {
	struct weston_seat base;
	struct wayland_compositor *compositor;
	struct wl_list link;

	struct {
		struct wl_seat *seat;
		struct wl_pointer *pointer;
		struct wl_keyboard *keyboard;
		struct wl_touch *touch;

		struct {
			struct wl_surface *surface;
			int32_t hx, hy;
		} cursor;
	} parent;

	uint32_t key_serial;
	uint32_t enter_serial;
	int focus;
	struct wayland_output *output;
	struct wayland_output *keyboard_focus;
};

struct gl_renderer_interface *gl_renderer;

static void
frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
	struct weston_output *output = data;

	wl_callback_destroy(callback);
	weston_output_finish_frame(output, time);
}

static const struct wl_callback_listener frame_listener = {
	frame_done
};

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static void
draw_initial_frame(struct wayland_output *output)
{
	struct wayland_compositor *c =
		(struct wayland_compositor *) output->base.compositor;
	struct wl_shm *shm = c->parent.shm;
	struct wl_surface *surface = output->parent.surface;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;

	int width, height, stride;
	int size;
	int fd;
	void *data;

	if (output->frame) {
		width = frame_width(output->frame);
		height = frame_height(output->frame);
	} else {
		width = output->mode.width;
		height = output->mode.height;
	}

	stride = width * 4;
	size = height * stride;

	fd = os_create_anonymous_file(size);
	if (fd < 0) {
		perror("os_create_anonymous_file");
		return;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return;
	}

	pool = wl_shm_create_pool(shm, fd, size);

	buffer = wl_shm_pool_create_buffer(pool, 0,
					   width, height,
					   stride,
					   WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &buffer_listener, buffer);
	wl_shm_pool_destroy(pool);
	close(fd);

	memset(data, 0, size);

	wl_surface_attach(surface, buffer, 0, 0);

	/* We only need to damage some part, as its only transparant
	 * pixels anyway. */
	wl_surface_damage(surface, 0, 0, 1, 1);
}

static void
wayland_output_update_gl_border(struct wayland_output *output)
{
	int32_t ix, iy, iwidth, iheight, fwidth, fheight;
	cairo_t *cr;
	
	if (!output->frame)
		return;
	if (!(frame_status(output->frame) & FRAME_STATUS_REPAINT))
		return;
	
	fwidth = frame_width(output->frame);
	fheight = frame_height(output->frame);
	frame_interior(output->frame, &ix, &iy, &iwidth, &iheight);

	if (!output->gl.border.top)
		output->gl.border.top =
			cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
						   fwidth, iy);
	cr = cairo_create(output->gl.border.top);
	frame_repaint(output->frame, cr);
	cairo_destroy(cr);
	gl_renderer->output_set_border(&output->base, GL_RENDERER_BORDER_TOP,
				       fwidth, iy,
				       cairo_image_surface_get_stride(output->gl.border.top) / 4,
				       cairo_image_surface_get_data(output->gl.border.top));


	if (!output->gl.border.left)
		output->gl.border.left =
			cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
						   ix, 1);
	cr = cairo_create(output->gl.border.left);
	cairo_translate(cr, 0, -iy);
	frame_repaint(output->frame, cr);
	cairo_destroy(cr);
	gl_renderer->output_set_border(&output->base, GL_RENDERER_BORDER_LEFT,
				       ix, 1,
				       cairo_image_surface_get_stride(output->gl.border.left) / 4,
				       cairo_image_surface_get_data(output->gl.border.left));


	if (!output->gl.border.right)
		output->gl.border.right =
			cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
						   fwidth - (ix + iwidth), 1);
	cr = cairo_create(output->gl.border.right);
	cairo_translate(cr, -(iwidth + ix), -iy);
	frame_repaint(output->frame, cr);
	cairo_destroy(cr);
	gl_renderer->output_set_border(&output->base, GL_RENDERER_BORDER_RIGHT,
				       fwidth - (ix + iwidth), 1,
				       cairo_image_surface_get_stride(output->gl.border.right) / 4,
				       cairo_image_surface_get_data(output->gl.border.right));


	if (!output->gl.border.bottom)
		output->gl.border.bottom =
			cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
						   fwidth, fheight - (iy + iheight));
	cr = cairo_create(output->gl.border.bottom);
	cairo_translate(cr, 0, -(iy + iheight));
	frame_repaint(output->frame, cr);
	cairo_destroy(cr);
	gl_renderer->output_set_border(&output->base, GL_RENDERER_BORDER_BOTTOM,
				       fwidth, fheight - (iy + iheight),
				       cairo_image_surface_get_stride(output->gl.border.bottom) / 4,
				       cairo_image_surface_get_data(output->gl.border.bottom));
}

static void
wayland_output_start_repaint_loop(struct weston_output *output_base)
{
	struct wayland_output *output = (struct wayland_output *) output_base;
	struct wayland_compositor *wc =
		(struct wayland_compositor *)output->base.compositor;
	struct wl_callback *callback;

	/* If this is the initial frame, we need to attach a buffer so that
	 * the compositor can map the surface and include it in its render
	 * loop. If the surface doesn't end up in the render loop, the frame
	 * callback won't be invoked. The buffer is transparent and of the
	 * same size as the future real output buffer. */
	if (output->parent.draw_initial_frame) {
		output->parent.draw_initial_frame = 0;

		draw_initial_frame(output);
	}

	callback = wl_surface_frame(output->parent.surface);
	wl_callback_add_listener(callback, &frame_listener, output);
	wl_surface_commit(output->parent.surface);
	wl_display_flush(wc->parent.wl_display);
}

static void
wayland_output_repaint(struct weston_output *output_base,
		       pixman_region32_t *damage)
{
	struct wayland_output *output = (struct wayland_output *) output_base;
	struct weston_compositor *ec = output->base.compositor;
	struct wl_callback *callback;

	callback = wl_surface_frame(output->parent.surface);
	wl_callback_add_listener(callback, &frame_listener, output);

	wayland_output_update_gl_border(output);

	ec->renderer->repaint_output(&output->base, damage);

	pixman_region32_subtract(&ec->primary_plane.damage,
				 &ec->primary_plane.damage, damage);

}

static void
wayland_output_destroy(struct weston_output *output_base)
{
	struct wayland_output *output = (struct wayland_output *) output_base;

	gl_renderer->output_destroy(output_base);

	wl_egl_window_destroy(output->parent.egl_window);
	wl_surface_destroy(output->parent.surface);
	wl_shell_surface_destroy(output->parent.shell_surface);

	if (output->frame) {
		frame_destroy(output->frame);
		cairo_surface_destroy(output->border.top);
		cairo_surface_destroy(output->border.left);
		cairo_surface_destroy(output->border.right);
		cairo_surface_destroy(output->border.bottom);
	}

	weston_output_destroy(&output->base);
	free(output);

	return;
}

static const struct wl_shell_surface_listener shell_surface_listener;

static int
wayland_compositor_create_output(struct wayland_compositor *c,
				 int width, int height)
{
	struct wayland_output *output;
	int32_t fx, fy, fwidth, fheight;
	struct wl_region *region;

	output = zalloc(sizeof *output);
	if (output == NULL)
		return -1;

	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = width;
	output->mode.height = height;
	output->mode.refresh = 60;
	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	output->base.current_mode = &output->mode;
	weston_output_init(&output->base, &c->base, 0, 0, width, height,
			   WL_OUTPUT_TRANSFORM_NORMAL, 1);

	output->base.make = "waywayland";
	output->base.model = "none";

	weston_output_move(&output->base, 0, 0);

	if (!c->theme)
		c->theme = theme_create();
	output->frame = frame_create(c->theme, width, height,
				     FRAME_BUTTON_CLOSE, "Weston");
	frame_resize_inside(output->frame, width, height);
	frame_interior(output->frame, &fx, &fy, NULL, NULL);
	fwidth = frame_width(output->frame);
	fheight = frame_height(output->frame);

	weston_log("Creating %dx%d wayland output (%dx%d actual)\n",
		   width, height, fwidth, fheight);

	output->parent.surface =
		wl_compositor_create_surface(c->parent.compositor);
	wl_surface_set_user_data(output->parent.surface, output);

	output->parent.egl_window =
		wl_egl_window_create(output->parent.surface,
				     fwidth, fheight);
	if (!output->parent.egl_window) {
		weston_log("failure to create wl_egl_window\n");
		goto cleanup_output;
	}

	if (gl_renderer->output_create(&output->base,
			output->parent.egl_window) < 0)
		goto cleanup_window;

	output->base.border.left = fx;
	output->base.border.top = fy;
	output->base.border.right = fwidth - width - fx;
	output->base.border.bottom = fheight - height - fy;

	frame_input_rect(output->frame, &fx, &fy, &fwidth, &fheight);
	region = wl_compositor_create_region(c->parent.compositor);
	wl_region_add(region, fx, fy, fwidth, fheight);
	wl_surface_set_input_region(output->parent.surface, region);
	wl_region_destroy(region);

	frame_opaque_rect(output->frame, &fx, &fy, &fwidth, &fheight);
	region = wl_compositor_create_region(c->parent.compositor);
	wl_region_add(region, fx, fy, fwidth, fheight);
	wl_surface_set_opaque_region(output->parent.surface, region);
	wl_region_destroy(region);

	output->parent.draw_initial_frame = 1;
	output->parent.shell_surface =
		wl_shell_get_shell_surface(c->parent.shell,
					   output->parent.surface);
	wl_shell_surface_add_listener(output->parent.shell_surface,
				      &shell_surface_listener, output);
	wl_shell_surface_set_toplevel(output->parent.shell_surface);

	output->base.start_repaint_loop = wayland_output_start_repaint_loop;
	output->base.repaint = wayland_output_repaint;
	output->base.destroy = wayland_output_destroy;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = NULL;

	wl_list_insert(c->base.output_list.prev, &output->base.link);

	return 0;

cleanup_window:
	wl_egl_window_destroy(output->parent.egl_window);
cleanup_output:
	/* FIXME: cleanup weston_output */
	free(output);

	return -1;
}

static void
shell_surface_ping(void *data, struct wl_shell_surface *shell_surface,
		   uint32_t serial)
{
	wl_shell_surface_pong(shell_surface, serial);
}

static void
shell_surface_configure(void *data, struct wl_shell_surface *shell_surface,
			uint32_t edges, int32_t width, int32_t height)
{
	/* FIXME: implement resizing */
}

static void
shell_surface_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	shell_surface_ping,
	shell_surface_configure,
	shell_surface_popup_done
};

/* Events received from the wayland-server this compositor is client of: */

/* parent output interface */
static void
display_handle_geometry(void *data,
			struct wl_output *wl_output,
			int x,
			int y,
			int physical_width,
			int physical_height,
			int subpixel,
			const char *make,
			const char *model,
			int transform)
{
	struct wayland_compositor *c = data;

	c->parent.screen_allocation.x = x;
	c->parent.screen_allocation.y = y;
}

static void
display_handle_mode(void *data,
		    struct wl_output *wl_output,
		    uint32_t flags,
		    int width,
		    int height,
		    int refresh)
{
	struct wayland_compositor *c = data;

	c->parent.screen_allocation.width = width;
	c->parent.screen_allocation.height = height;
}

static const struct wl_output_listener output_listener = {
	display_handle_geometry,
	display_handle_mode
};

/* parent input interface */
static void
input_set_cursor(struct wayland_input *input)
{

	struct wl_buffer *buffer;
	struct wl_cursor_image *image;

	if (!input->compositor->cursor)
		return; /* Couldn't load the cursor. Can't set it */

	image = input->compositor->cursor->images[0];
	buffer = wl_cursor_image_get_buffer(image);


	wl_pointer_set_cursor(input->parent.pointer, input->enter_serial,
			      input->parent.cursor.surface,
			      image->hotspot_x, image->hotspot_y);

	wl_surface_attach(input->parent.cursor.surface, buffer, 0, 0);
	wl_surface_damage(input->parent.cursor.surface, 0, 0,
			  image->width, image->height);
	wl_surface_commit(input->parent.cursor.surface);
}

static void
input_handle_pointer_enter(void *data, struct wl_pointer *pointer,
			   uint32_t serial, struct wl_surface *surface,
			   wl_fixed_t x, wl_fixed_t y)
{
	struct wayland_input *input = data;
	int32_t fx, fy;
	enum theme_location location;

	/* XXX: If we get a modifier event immediately before the focus,
	 *      we should try to keep the same serial. */
	input->enter_serial = serial;
	input->output = wl_surface_get_user_data(surface);

	if (input->output->frame) {
		location = frame_pointer_enter(input->output->frame, input,
					       wl_fixed_to_int(x),
					       wl_fixed_to_int(y));
		frame_interior(input->output->frame, &fx, &fy, NULL, NULL);
		x -= wl_fixed_from_int(fx);
		y -= wl_fixed_from_int(fy);

		if (frame_status(input->output->frame) & FRAME_STATUS_REPAINT)
			weston_output_schedule_repaint(&input->output->base);
	} else {
		location = THEME_LOCATION_CLIENT_AREA;
	}

	if (location == THEME_LOCATION_CLIENT_AREA) {
		input->focus = 1;
		notify_pointer_focus(&input->base, &input->output->base, x, y);
		wl_pointer_set_cursor(input->parent.pointer,
				      input->enter_serial, NULL, 0, 0);
	} else {
		input->focus = 0;
		notify_pointer_focus(&input->base, NULL, 0, 0);
		input_set_cursor(input);
	}
}

static void
input_handle_pointer_leave(void *data, struct wl_pointer *pointer,
			   uint32_t serial, struct wl_surface *surface)
{
	struct wayland_input *input = data;

	if (input->output->frame) {
		frame_pointer_leave(input->output->frame, input);

		if (frame_status(input->output->frame) & FRAME_STATUS_REPAINT)
			weston_output_schedule_repaint(&input->output->base);
	}

	notify_pointer_focus(&input->base, NULL, 0, 0);
	input->output = NULL;
	input->focus = 0;
}

static void
input_handle_motion(void *data, struct wl_pointer *pointer,
		    uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	struct wayland_input *input = data;
	int32_t fx, fy;
	enum theme_location location;

	if (input->output->frame) {
		location = frame_pointer_motion(input->output->frame, input,
						wl_fixed_to_int(x),
						wl_fixed_to_int(y));
		frame_interior(input->output->frame, &fx, &fy, NULL, NULL);
		x -= wl_fixed_from_int(fx);
		y -= wl_fixed_from_int(fy);

		if (frame_status(input->output->frame) & FRAME_STATUS_REPAINT)
			weston_output_schedule_repaint(&input->output->base);
	} else {
		location = THEME_LOCATION_CLIENT_AREA;
	}

	if (input->focus && location != THEME_LOCATION_CLIENT_AREA) {
		input_set_cursor(input);
		notify_pointer_focus(&input->base, NULL, 0, 0);
		input->focus = 0;
	} else if (!input->focus && location == THEME_LOCATION_CLIENT_AREA) {
		wl_pointer_set_cursor(input->parent.pointer,
				      input->enter_serial, NULL, 0, 0);
		notify_pointer_focus(&input->base, &input->output->base,
				     x - wl_fixed_from_int(fx),
				     y - wl_fixed_from_int(fy));
		input->focus = 1;
	}

	if (location == THEME_LOCATION_CLIENT_AREA)
		notify_motion_absolute(&input->base, time, x, y);
}

static void
input_handle_button(void *data, struct wl_pointer *pointer,
		    uint32_t serial, uint32_t time, uint32_t button,
		    uint32_t state_w)
{
	struct wayland_input *input = data;
	enum wl_pointer_button_state state = state_w;
	enum frame_button_state fstate;
	enum theme_location location;

	if (input->output->frame) {
		fstate = state == WL_POINTER_BUTTON_STATE_PRESSED ?
			FRAME_BUTTON_PRESSED : FRAME_BUTTON_RELEASED;

		location = frame_pointer_button(input->output->frame, input,
						button, fstate);

		if (frame_status(input->output->frame) & FRAME_STATUS_MOVE) {
			
			wl_shell_surface_move(input->output->parent.shell_surface,
					      input->parent.seat, serial);
			frame_status_clear(input->output->frame,
					   FRAME_STATUS_MOVE);
			return;
		}

		if (frame_status(input->output->frame) & FRAME_STATUS_CLOSE)
			wl_display_terminate(input->compositor->base.wl_display);

		if (frame_status(input->output->frame) & FRAME_STATUS_REPAINT)
			weston_output_schedule_repaint(&input->output->base);
	} else {
		location = THEME_LOCATION_CLIENT_AREA;
	}

	if (location == THEME_LOCATION_CLIENT_AREA)
		notify_button(&input->base, time, button, state);
}

static void
input_handle_axis(void *data, struct wl_pointer *pointer,
		  uint32_t time, uint32_t axis, wl_fixed_t value)
{
	struct wayland_input *input = data;

	notify_axis(&input->base, time, axis, value);
}

static const struct wl_pointer_listener pointer_listener = {
	input_handle_pointer_enter,
	input_handle_pointer_leave,
	input_handle_motion,
	input_handle_button,
	input_handle_axis,
};

static void
input_handle_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
		    int fd, uint32_t size)
{
	struct wayland_input *input = data;
	struct xkb_keymap *keymap;
	char *map_str;

	if (!data) {
		close(fd);
		return;
	}

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}

	map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_str == MAP_FAILED) {
		close(fd);
		return;
	}

	keymap = xkb_map_new_from_string(input->compositor->base.xkb_context,
					 map_str,
					 XKB_KEYMAP_FORMAT_TEXT_V1,
					 0);
	munmap(map_str, size);
	close(fd);

	if (!keymap) {
		weston_log("failed to compile keymap\n");
		return;
	}

	weston_seat_init_keyboard(&input->base, keymap);
	xkb_map_unref(keymap);
}

static void
input_handle_keyboard_enter(void *data,
			    struct wl_keyboard *keyboard,
			    uint32_t serial,
			    struct wl_surface *surface,
			    struct wl_array *keys)
{
	struct wayland_input *input = data;
	struct wayland_output *focus;

	focus = input->keyboard_focus;
	if (focus) {
		/* This shouldn't happen */
		focus->keyboard_count--;
		if (!focus->keyboard_count && focus->frame)
			frame_unset_flag(focus->frame, FRAME_FLAG_ACTIVE);
		if (frame_status(focus->frame) & FRAME_STATUS_REPAINT)
			weston_output_schedule_repaint(&focus->base);
	}

	input->keyboard_focus = wl_surface_get_user_data(surface);
	input->keyboard_focus->keyboard_count++;

	focus = input->keyboard_focus;
	if (focus->frame) {
		frame_set_flag(focus->frame, FRAME_FLAG_ACTIVE);
		if (frame_status(focus->frame) & FRAME_STATUS_REPAINT)
			weston_output_schedule_repaint(&focus->base);
	}


	/* XXX: If we get a modifier event immediately before the focus,
	 *      we should try to keep the same serial. */
	notify_keyboard_focus_in(&input->base, keys,
				 STATE_UPDATE_AUTOMATIC);
}

static void
input_handle_keyboard_leave(void *data,
			    struct wl_keyboard *keyboard,
			    uint32_t serial,
			    struct wl_surface *surface)
{
	struct wayland_input *input = data;
	struct wayland_output *focus;

	notify_keyboard_focus_out(&input->base);

	focus = input->keyboard_focus;
	if (!focus)
		return; /* This shouldn't happen */
	
	focus->keyboard_count--;
	if (!focus->keyboard_count && focus->frame) {
		frame_unset_flag(focus->frame, FRAME_FLAG_ACTIVE);
		if (frame_status(focus->frame) & FRAME_STATUS_REPAINT)
			weston_output_schedule_repaint(&focus->base);
	}

	input->keyboard_focus = NULL;
}

static void
input_handle_key(void *data, struct wl_keyboard *keyboard,
		 uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	struct wayland_input *input = data;

	input->key_serial = serial;
	notify_key(&input->base, time, key,
		   state ? WL_KEYBOARD_KEY_STATE_PRESSED :
			   WL_KEYBOARD_KEY_STATE_RELEASED,
		   STATE_UPDATE_NONE);
}

static void
input_handle_modifiers(void *data, struct wl_keyboard *keyboard,
		       uint32_t serial_in, uint32_t mods_depressed,
		       uint32_t mods_latched, uint32_t mods_locked,
		       uint32_t group)
{
	struct wayland_input *input = data;
	struct wayland_compositor *c = input->compositor;
	uint32_t serial_out;

	/* If we get a key event followed by a modifier event with the
	 * same serial number, then we try to preserve those semantics by
	 * reusing the same serial number on the way out too. */
	if (serial_in == input->key_serial)
		serial_out = wl_display_get_serial(c->base.wl_display);
	else
		serial_out = wl_display_next_serial(c->base.wl_display);

	xkb_state_update_mask(input->base.xkb_state.state,
			      mods_depressed, mods_latched,
			      mods_locked, 0, 0, group);
	notify_modifiers(&input->base, serial_out);
}

static const struct wl_keyboard_listener keyboard_listener = {
	input_handle_keymap,
	input_handle_keyboard_enter,
	input_handle_keyboard_leave,
	input_handle_key,
	input_handle_modifiers,
};

static void
input_handle_capabilities(void *data, struct wl_seat *seat,
		          enum wl_seat_capability caps)
{
	struct wayland_input *input = data;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->parent.pointer) {
		input->parent.pointer = wl_seat_get_pointer(seat);
		wl_pointer_set_user_data(input->parent.pointer, input);
		wl_pointer_add_listener(input->parent.pointer,
					&pointer_listener, input);
		weston_seat_init_pointer(&input->base);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && input->parent.pointer) {
		wl_pointer_destroy(input->parent.pointer);
		input->parent.pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !input->parent.keyboard) {
		input->parent.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_set_user_data(input->parent.keyboard, input);
		wl_keyboard_add_listener(input->parent.keyboard,
					 &keyboard_listener, input);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && input->parent.keyboard) {
		wl_keyboard_destroy(input->parent.keyboard);
		input->parent.keyboard = NULL;
	}
}

static const struct wl_seat_listener seat_listener = {
	input_handle_capabilities,
};

static void
display_add_seat(struct wayland_compositor *c, uint32_t id)
{
	struct wayland_input *input;

	input = zalloc(sizeof *input);
	if (input == NULL)
		return;

	weston_seat_init(&input->base, &c->base, "default");
	input->compositor = c;
	input->parent.seat = wl_registry_bind(c->parent.registry, id,
					      &wl_seat_interface, 1);
	wl_list_insert(c->inputs.prev, &input->link);

	wl_seat_add_listener(input->parent.seat, &seat_listener, input);
	wl_seat_set_user_data(input->parent.seat, input);

	input->parent.cursor.surface =
		wl_compositor_create_surface(c->parent.compositor);
}

static void
registry_handle_global(void *data, struct wl_registry *registry, uint32_t name,
		       const char *interface, uint32_t version)
{
	struct wayland_compositor *c = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		c->parent.compositor =
			wl_registry_bind(registry, name,
					 &wl_compositor_interface, 1);
	} else if (strcmp(interface, "wl_output") == 0) {
		c->parent.output =
			wl_registry_bind(registry, name,
					 &wl_output_interface, 1);
		wl_output_add_listener(c->parent.output, &output_listener, c);
	} else if (strcmp(interface, "wl_shell") == 0) {
		c->parent.shell =
			wl_registry_bind(registry, name,
					 &wl_shell_interface, 1);
	} else if (strcmp(interface, "wl_seat") == 0) {
		display_add_seat(c, name);
	} else if (strcmp(interface, "wl_shm") == 0) {
		c->parent.shm =
			wl_registry_bind(registry, name, &wl_shm_interface, 1);
	}
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global
};

static int
wayland_compositor_handle_event(int fd, uint32_t mask, void *data)
{
	struct wayland_compositor *c = data;
	int count = 0;

	if (mask & WL_EVENT_READABLE)
		count = wl_display_dispatch(c->parent.wl_display);
	if (mask & WL_EVENT_WRITABLE)
		wl_display_flush(c->parent.wl_display);

	if (mask == 0) {
		count = wl_display_dispatch_pending(c->parent.wl_display);
		wl_display_flush(c->parent.wl_display);
	}

	return count;
}

static void
wayland_restore(struct weston_compositor *ec)
{
}

static void
wayland_destroy(struct weston_compositor *ec)
{
	struct wayland_compositor *c = (struct wayland_compositor *) ec;

	ec->renderer->destroy(ec);

	weston_compositor_shutdown(ec);

	if (c->parent.shm)
		wl_shm_destroy(c->parent.shm);

	free(ec);
}

static void create_cursor(struct wayland_compositor *c);

static struct weston_compositor *
wayland_compositor_create(struct wl_display *display,
			  int width, int height, const char *display_name,
			  int *argc, char *argv[],
			  struct weston_config *config)
{
	struct wayland_compositor *c;
	struct wl_event_loop *loop;
	int fd;

	c = zalloc(sizeof *c);
	if (c == NULL)
		return NULL;

	if (weston_compositor_init(&c->base, display, argc, argv,
				   config) < 0)
		goto err_free;

	c->parent.wl_display = wl_display_connect(display_name);

	if (c->parent.wl_display == NULL) {
		weston_log("failed to create display: %m\n");
		goto err_compositor;
	}

	wl_list_init(&c->inputs);
	c->parent.registry = wl_display_get_registry(c->parent.wl_display);
	wl_registry_add_listener(c->parent.registry, &registry_listener, c);
	wl_display_roundtrip(c->parent.wl_display);

	create_cursor(c);

	c->base.wl_display = display;

	gl_renderer = weston_load_module("gl-renderer.so",
					 "gl_renderer_interface");
	if (!gl_renderer)
		goto err_display;

	if (gl_renderer->create(&c->base, c->parent.wl_display,
			gl_renderer->alpha_attribs,
			NULL) < 0)
		goto err_display;

	c->base.destroy = wayland_destroy;
	c->base.restore = wayland_restore;

	/* requires border fields */
	if (wayland_compositor_create_output(c, width, height) < 0)
		goto err_gl;

	loop = wl_display_get_event_loop(c->base.wl_display);

	fd = wl_display_get_fd(c->parent.wl_display);
	c->parent.wl_source =
		wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				     wayland_compositor_handle_event, c);
	if (c->parent.wl_source == NULL)
		goto err_gl;

	wl_event_source_check(c->parent.wl_source);

	return &c->base;

err_gl:
	c->base.renderer->destroy(&c->base);
err_display:
	wl_display_disconnect(c->parent.wl_display);
err_compositor:
	weston_compositor_shutdown(&c->base);
err_free:
	free(c);
	return NULL;
}

static const char *left_ptrs[] = {
	"left_ptr",
	"default",
	"top_left_arrow",
	"left-arrow"
};

static void
create_cursor(struct wayland_compositor *c)
{
	struct weston_config *config;
	struct weston_config_section *s;
	int size;
	char *theme = NULL;
	unsigned int i;

	config = weston_config_parse("weston.ini");
	s = weston_config_get_section(config, "shell", NULL, NULL);
	weston_config_section_get_string(s, "cursor-theme", &theme, NULL);
	weston_config_section_get_int(s, "cursor-size", &size, 32);
	weston_config_destroy(config);

	c->cursor_theme = wl_cursor_theme_load(theme, size, c->parent.shm);

	c->cursor = NULL;
	for (i = 0; !c->cursor && i < ARRAY_LENGTH(left_ptrs); ++i)
		c->cursor = wl_cursor_theme_get_cursor(c->cursor_theme,
						       left_ptrs[i]);
	if (!c->cursor) {
		fprintf(stderr, "could not load left cursor\n");
		return;
	}
}

WL_EXPORT struct weston_compositor *
backend_init(struct wl_display *display, int *argc, char *argv[],
	     struct weston_config *config)
{
	int width = 1024, height = 640;
	char *display_name = NULL;

	const struct weston_option wayland_options[] = {
		{ WESTON_OPTION_INTEGER, "width", 0, &width },
		{ WESTON_OPTION_INTEGER, "height", 0, &height },
		{ WESTON_OPTION_STRING, "display", 0, &display_name },
	};

	parse_options(wayland_options,
		      ARRAY_LENGTH(wayland_options), argc, argv);

	return wayland_compositor_create(display, width, height, display_name,
					 argc, argv, config);
}
