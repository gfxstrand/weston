/*
 * Copyright © 2013 Jason Ekstrand
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

#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "compositor.h"
#include "fullscreen-shell-server-protocol.h"

struct fullscreen_shell {
	struct wl_client *client;
	struct wl_listener client_destroyed;
	struct weston_compositor *compositor;

	struct weston_layer layer;
	struct wl_list output_list;
	struct wl_listener output_created_listener;

	struct wl_listener seat_created_listener;
};

struct fs_output {
	struct fullscreen_shell *shell;
	struct wl_list link;

	struct weston_output *output;
	struct wl_listener output_destroyed;

	struct weston_surface *surface;
	struct wl_listener surface_destroyed;
	struct weston_view *view;
	struct weston_view *black_view;
	struct weston_transform transform; /* matrix from x, y */

	enum wl_fullscreen_shell_present_method method;
	uint32_t framerate;
};

struct pointer_focus_listener {
	struct fullscreen_shell *shell;
	struct wl_listener pointer_focus;
	struct wl_listener seat_caps;
	struct wl_listener seat_destroyed;
};

static void
pointer_focus_changed(struct wl_listener *listener, void *data)
{
	struct weston_pointer *pointer = data;

	if (pointer->focus && pointer->focus->surface->resource)
		weston_surface_activate(pointer->focus->surface, pointer->seat);
}

static void
seat_caps_changed(struct wl_listener *l, void *data)
{
	struct weston_seat *seat = data;
	struct pointer_focus_listener *listener;
	struct fs_output *fsout;

	listener = container_of(l, struct pointer_focus_listener, seat_caps);

	/* no pointer */
	if (seat->pointer) {
		if (!listener->pointer_focus.link.prev) {
			wl_signal_add(&seat->pointer->focus_signal,
				      &listener->pointer_focus);
		}
	} else {
		if (listener->pointer_focus.link.prev) {
			wl_list_remove(&listener->pointer_focus.link);
		}
	}

	if (seat->keyboard && seat->keyboard->focus != NULL) {
		wl_list_for_each(fsout, &listener->shell->output_list, link) {
			if (fsout->surface) {
				weston_surface_activate(fsout->surface, seat);
				return;
			}
		}
	}
}

static void
seat_destroyed(struct wl_listener *l, void *data)
{
	struct pointer_focus_listener *listener;

	listener = container_of(l, struct pointer_focus_listener,
				seat_destroyed);

	free(listener);
}

static void
seat_created(struct wl_listener *l, void *data)
{
	struct weston_seat *seat = data;
	struct pointer_focus_listener *listener;

	listener = malloc(sizeof *listener);
	if (!listener)
		return;
	memset(listener, 0, sizeof *listener);

	listener->shell = container_of(l, struct fullscreen_shell,
				       seat_created_listener);
	listener->pointer_focus.notify = pointer_focus_changed;
	listener->seat_caps.notify = seat_caps_changed;
	listener->seat_destroyed.notify = seat_destroyed;

	wl_signal_add(&seat->destroy_signal, &listener->seat_destroyed);
	wl_signal_add(&seat->updated_caps_signal, &listener->seat_caps);

	seat_caps_changed(&listener->seat_caps, seat);
}

static void
black_surface_configure(struct weston_surface *es, int32_t sx, int32_t sy)
{
}

static struct weston_view *
create_black_surface(struct weston_compositor *ec, struct fs_output *fsout,
		     float x, float y, int w, int h)
{
	struct weston_surface *surface = NULL;
	struct weston_view *view;

	surface = weston_surface_create(ec);
	if (surface == NULL) {
		weston_log("no memory\n");
		return NULL;
	}
	view = weston_view_create(surface);
	if (!view) {
		weston_surface_destroy(surface);
		weston_log("no memory\n");
		return NULL;
	}

	surface->configure = black_surface_configure;
	surface->configure_private = fsout;
	weston_surface_set_color(surface, 0.0f, 0.0f, 0.0f, 1.0f);
	pixman_region32_fini(&surface->opaque);
	pixman_region32_init_rect(&surface->opaque, 0, 0, w, h);
	pixman_region32_fini(&surface->input);
	pixman_region32_init_rect(&surface->input, 0, 0, w, h);

	weston_surface_set_size(surface, w, h);
	weston_view_set_position(view, x, y);

	return view;
}

static void
fs_output_destroy(struct fs_output *fsout)
{
	wl_list_remove(&fsout->link);

	if (fsout->output)
		wl_list_remove(&fsout->output_destroyed.link);

	if (fsout->view) {
		weston_view_destroy(fsout->view);
		wl_list_remove(&fsout->surface_destroyed.link);
	}
}

static void
output_destroyed(struct wl_listener *listener, void *data)
{
	struct fs_output *output = container_of(listener,
						struct fs_output,
						output_destroyed);
	fs_output_destroy(output);
}

static void
surface_destroyed(struct wl_listener *listener, void *data)
{
	struct fs_output *output = container_of(listener,
						struct fs_output,
						surface_destroyed);
	output->surface = NULL;
	output->view = NULL;
}

static struct fs_output *
fs_output_create(struct fullscreen_shell *shell, struct weston_output *output)
{
	struct fs_output *fsout;

	fsout = malloc(sizeof *fsout);
	if (!fsout)
		return NULL;
	memset(fsout, 0, sizeof *fsout);

	fsout->shell = shell;
	wl_list_insert(&shell->output_list, &fsout->link);

	fsout->output = output;
	fsout->output_destroyed.notify = output_destroyed;
	wl_signal_add(&output->destroy_signal, &fsout->output_destroyed);

	fsout->surface_destroyed.notify = surface_destroyed;
	fsout->black_view = create_black_surface(shell->compositor, fsout,
						 output->x, output->y,
						 output->width, output->height);
	wl_list_insert(&shell->layer.view_list,
		       &fsout->black_view->layer_link);
	wl_list_init(&fsout->transform.link);
	return fsout;
}

static struct fs_output *
fs_output_for_output(struct weston_output *output)
{
	struct wl_listener *listener;

	if (!output)
		return NULL;

	listener = wl_signal_get(&output->destroy_signal, output_destroyed);

	return container_of(listener, struct fs_output, output_destroyed);
}

static void
restore_output_mode(struct weston_output *output)
{
	if (output->current_mode != output->original_mode ||
	    (int32_t)output->current_scale != output->original_scale)
		weston_output_switch_mode(output,
					  output->original_mode,
					  output->original_scale,
					  WESTON_MODE_SWITCH_RESTORE_NATIVE);
}

/*
 * Returns the bounding box of a surface and all its sub-surfaces,
 * in the surface coordinates system. */
static void
surface_subsurfaces_boundingbox(struct weston_surface *surface, int32_t *x,
				int32_t *y, int32_t *w, int32_t *h) {
	pixman_region32_t region;
	pixman_box32_t *box;
	struct weston_subsurface *subsurface;

	pixman_region32_init_rect(&region, 0, 0,
	                          surface->width,
	                          surface->height);

	wl_list_for_each(subsurface, &surface->subsurface_list, parent_link) {
		pixman_region32_union_rect(&region, &region,
		                           subsurface->position.x,
		                           subsurface->position.y,
		                           subsurface->surface->width,
		                           subsurface->surface->height);
	}

	box = pixman_region32_extents(&region);
	if (x)
		*x = box->x1;
	if (y)
		*y = box->y1;
	if (w)
		*w = box->x2 - box->x1;
	if (h)
		*h = box->y2 - box->y1;

	pixman_region32_fini(&region);
}

static void
fs_output_center_view(struct fs_output *fsout)
{
	int32_t surf_x, surf_y, surf_width, surf_height;
	float x, y;
	struct weston_output *output = fsout->output;

	surface_subsurfaces_boundingbox(fsout->view->surface, &surf_x, &surf_y,
					&surf_width, &surf_height);

	x = output->x + (output->width - surf_width) / 2 - surf_x / 2;
	y = output->y + (output->height - surf_height) / 2 - surf_y / 2;

	weston_view_set_position(fsout->view, x, y);
}

static void
fs_output_scale_view(struct fs_output *fsout, float width, float height)
{
	float x, y;
	int32_t surf_x, surf_y, surf_width, surf_height;
	struct weston_matrix *matrix;
	struct weston_view *view = fsout->view;
	struct weston_output *output = fsout->output;

	surface_subsurfaces_boundingbox(view->surface, &surf_x, &surf_y,
					&surf_width, &surf_height);

	if (output->width == surf_width && output->height == surf_height) {
		weston_view_set_position(view,
					 fsout->output->x - surf_x,
					 fsout->output->y - surf_y);
	} else {
		matrix = &fsout->transform.matrix;
		weston_matrix_init(matrix);

		weston_matrix_scale(matrix, width / surf_width,
				    height / surf_height, 1);
		wl_list_remove(&fsout->transform.link);
		wl_list_insert(&fsout->view->geometry.transformation_list,
			       &fsout->transform.link);

		x = output->x + (output->width - width) / 2 - surf_x;
		y = output->y + (output->height - height) / 2 - surf_y;

		weston_view_set_position(view, x, y);
	}
}

static void
configure_output(struct fs_output *fsout)
{
	struct weston_output *output = fsout->output;
	float output_aspect, surface_aspect;
	int32_t surf_x, surf_y, surf_width, surf_height;
	struct weston_mode mode;

	assert(fsout->view);

	if (fsout->method != WL_FULLSCREEN_SHELL_PRESENT_METHOD_DRIVER)
		restore_output_mode(fsout->output);

	wl_list_remove(&fsout->transform.link);
	wl_list_init(&fsout->transform.link);

	surface_subsurfaces_boundingbox(fsout->view->surface,
					&surf_x, &surf_y,
					&surf_width, &surf_height);

	output_aspect = (float) output->width / (float) output->height;
	surface_aspect = (float) surf_width / (float) surf_height;

	switch (fsout->method) {
	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_DEFAULT:
	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_CENTER:
		fs_output_center_view(fsout);
		break;

	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_ZOOM:
		if (output_aspect < surface_aspect)
			fs_output_scale_view(fsout,
					     output->width,
					     output->width / surface_aspect);
		else
			fs_output_scale_view(fsout,
					     output->height * surface_aspect,
					     output->height);
		break;

	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_ZOOM_CROP:
		if (output_aspect < surface_aspect)
			fs_output_scale_view(fsout,
					     output->height * surface_aspect,
					     output->height);
		else
			fs_output_scale_view(fsout,
					     output->width,
					     output->width / surface_aspect);
		break;

	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_STRETCH:
		fs_output_scale_view(fsout, output->width, output->height);
		break;

	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_DRIVER:
		mode.flags = 0;
		mode.width = surf_width * fsout->view->surface->buffer_viewport.scale;
		mode.height = surf_height * fsout->view->surface->buffer_viewport.scale;
		mode.refresh = fsout->framerate;

		if (weston_output_switch_mode(fsout->output, &mode,
					      fsout->view->surface->buffer_viewport.scale,
					      WESTON_MODE_SWITCH_SET_TEMPORARY) == 0) {
			weston_view_set_position(fsout->view,
						 fsout->output->x - surf_x,
						 fsout->output->y - surf_y);
			break;
		} else {
			restore_output_mode(fsout->output);
			fs_output_center_view(fsout);
		}
		break;

	default:
		break;
	}

	weston_view_set_position(fsout->black_view,
				 fsout->output->x - surf_x,
				 fsout->output->y - surf_y);
	weston_surface_set_size(fsout->black_view->surface,
				fsout->output->width,
				fsout->output->height);

	weston_output_schedule_repaint(fsout->output);
}

static void
configure_presented_surface(struct weston_surface *surface, int32_t sx,
			    int32_t sy)
{
	struct fullscreen_shell *shell = surface->configure_private;
	struct fs_output *fsout;

	if (surface->configure != configure_presented_surface)
		return;

	wl_list_for_each(fsout, &shell->output_list, link)
		if (fsout->view && fsout->view->surface == surface)
			configure_output(fsout);
}

static void
fs_output_set_surface(struct fs_output *fsout, struct weston_surface *surface,
		      enum wl_fullscreen_shell_present_method method,
		      uint32_t framerate)
{
	if (fsout->view && fsout->surface != surface) {
		wl_list_remove(&fsout->surface_destroyed.link);

		weston_view_destroy(fsout->view);
		fsout->view = NULL;

		if (wl_list_empty(&fsout->surface->views)) {
			fsout->surface->configure = NULL;
			fsout->surface->configure_private = NULL;
		}
	}

	fsout->method = method;
	fsout->framerate = framerate;

	if (surface && fsout->surface != surface) {
		if (!surface->configure) {
			surface->configure = configure_presented_surface;
			surface->configure_private = fsout->shell;
		}

		fsout->view = weston_view_create(surface);
		if (!fsout->view) {
			weston_log("no memory\n");
			return;
		}

		fsout->surface = surface;
		wl_signal_add(&surface->destroy_signal,
			      &fsout->surface_destroyed);
		wl_list_insert(&fsout->shell->layer.view_list,
			       &fsout->view->layer_link);
	}

	configure_output(fsout);
	weston_output_schedule_repaint(fsout->output);
}

static void
fullscreen_shell_present_surface(struct wl_client *client,
				 struct wl_resource *resource,
				 struct wl_resource *surface_res,
				 uint32_t method, uint32_t framerate,
				 struct wl_resource *output_res)
{
	struct fullscreen_shell *shell =
		wl_resource_get_user_data(resource);
	struct weston_output *output;
	struct weston_surface *surface;
	struct weston_seat *seat;
	struct fs_output *fsout;

	surface = surface_res ? wl_resource_get_user_data(surface_res) : NULL;

	switch(method) {
	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_DEFAULT:
	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_CENTER:
	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_ZOOM:
	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_ZOOM_CROP:
	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_STRETCH:
	case WL_FULLSCREEN_SHELL_PRESENT_METHOD_DRIVER:
		break;
	default:
		wl_resource_post_error(resource,
				       WL_FULLSCREEN_SHELL_ERROR_INVALID_METHOD,
				       "Invalid presentation method");
	}

	if (output_res) {
		output = wl_resource_get_user_data(output_res);
		fsout = fs_output_for_output(output);
		fs_output_set_surface(fsout, surface, method, framerate);
	} else {
		wl_list_for_each(fsout, &shell->output_list, link)
			fs_output_set_surface(fsout, surface, method, framerate);
	}

	if (surface) {
		wl_list_for_each(seat, &shell->compositor->seat_list, link) {
			if (seat->keyboard && seat->keyboard->focus == NULL)
				weston_surface_activate(surface, seat);
		}
	}
}

struct wl_fullscreen_shell_interface fullscreen_shell_implementation = {
	fullscreen_shell_present_surface,
};

static void
output_created(struct wl_listener *listener, void *data)
{
	struct fullscreen_shell *shell;

	shell = container_of(listener, struct fullscreen_shell,
			     output_created_listener);

	fs_output_create(shell, data);
}

static void
client_destroyed(struct wl_listener *listener, void *data)
{
	struct fullscreen_shell *shell = container_of(listener,
						     struct fullscreen_shell,
						     client_destroyed);
	shell->client = NULL;
}

static void
bind_fullscreen_shell(struct wl_client *client, void *data, uint32_t version,
		       uint32_t id)
{
	struct fullscreen_shell *shell = data;
	struct wl_resource *resource;

	if (shell->client != NULL && shell->client != client)
		return;
	else if (shell->client == NULL) {
		shell->client = client;
		wl_client_add_destroy_listener(client, &shell->client_destroyed);
	}

	resource = wl_resource_create(client, &wl_fullscreen_shell_interface,
				      1, id);
	wl_resource_set_implementation(resource,
				       &fullscreen_shell_implementation,
				       shell, NULL);
}

WL_EXPORT int
module_init(struct weston_compositor *compositor,
	    int *argc, char *argv[])
{
	struct fullscreen_shell *shell;
	struct weston_seat *seat;
	struct weston_output *output;

	shell = malloc(sizeof *shell);
	if (shell == NULL)
		return -1;

	memset(shell, 0, sizeof *shell);
	shell->compositor = compositor;

	shell->client_destroyed.notify = client_destroyed;

	weston_layer_init(&shell->layer, &compositor->cursor_layer.link);

	wl_list_init(&shell->output_list);
	shell->output_created_listener.notify = output_created;
	wl_signal_add(&compositor->output_created_signal,
		      &shell->output_created_listener);
	wl_list_for_each(output, &compositor->output_list, link)
		fs_output_create(shell, output);

	shell->seat_created_listener.notify = seat_created;
	wl_signal_add(&compositor->seat_created_signal,
		      &shell->seat_created_listener);
	wl_list_for_each(seat, &compositor->seat_list, link)
		seat_created(NULL, seat);

	wl_global_create(compositor->wl_display,
			 &wl_fullscreen_shell_interface, 1, shell,
			 bind_fullscreen_shell);

	return 0;
}
