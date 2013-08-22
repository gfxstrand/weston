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

#include "compositor.h"
#include "system-compositor-server-protocol.h"

struct system_compositor {
	struct wl_client *client;
	struct weston_compositor *compositor;

	struct wl_list surfaces_list;
	struct weston_layer layer;
};

struct sc_surface {
	struct weston_surface *surface;
	struct weston_surface *black_surface;
	struct wl_list link;

	enum wl_system_compositor_fullscreen_method method;
	struct weston_transform transform; /* matrix from x, y */
	uint32_t framerate;
	struct weston_output *output;
};

static void
black_surface_configure(struct weston_surface *es, int32_t sx, int32_t sy,
			int32_t width, int32_t height)
{
}

static struct weston_surface *
create_black_surface(struct weston_compositor *ec, struct sc_surface *scsurf,
		     float x, float y, int w, int h)
{
	struct weston_surface *surface = NULL;

	surface = weston_surface_create(ec);
	if (surface == NULL) {
		weston_log("no memory\n");
		return NULL;
	}

	surface->configure = black_surface_configure;
	surface->configure_private = scsurf;
	weston_surface_configure(surface, x, y, w, h);
	weston_surface_set_color(surface, 0.0, 0.0, 0.0, 1);
	pixman_region32_fini(&surface->opaque);
	pixman_region32_init_rect(&surface->opaque, 0, 0, w, h);
	pixman_region32_fini(&surface->input);
	pixman_region32_init_rect(&surface->input, 0, 0, w, h);

	return surface;
}

static struct sc_surface *
find_surface_for_output(struct system_compositor *syscomp,
			struct weston_output *output)
{
	struct sc_surface *scsurf;

	wl_list_for_each(scsurf, &syscomp->surfaces_list, link) {
		if (scsurf->output == output)
			return scsurf;
	}

	return NULL;
}

static void
restore_output_mode(struct weston_output *output)
{
	if (output->current != output->origin ||
	    (int32_t)output->scale != output->origin_scale)
		weston_output_switch_mode(output,
					  output->origin,
					  output->origin_scale);
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
	                          surface->geometry.width,
	                          surface->geometry.height);

	wl_list_for_each(subsurface, &surface->subsurface_list, parent_link) {
		pixman_region32_union_rect(&region, &region,
		                           subsurface->position.x,
		                           subsurface->position.y,
		                           subsurface->surface->geometry.width,
		                           subsurface->surface->geometry.height);
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
center_on_output(struct weston_surface *surface, struct weston_output *output)
{
	int32_t surf_x, surf_y, width, height;
	float x, y;

	surface_subsurfaces_boundingbox(surface, &surf_x, &surf_y, &width, &height);

	x = output->x + (output->width - width) / 2 - surf_x / 2;
	y = output->y + (output->height - height) / 2 - surf_y / 2;

	weston_surface_configure(surface, x, y, width, height);
}

static void
configure_presented_surface(struct weston_surface *surface, int32_t sx,
			    int32_t sy, int32_t width, int32_t height)
{
	struct sc_surface *scsurf;
	float scale, output_aspect, surface_aspect, x, y;
	int32_t surf_x, surf_y, surf_width, surf_height;
	struct weston_matrix *matrix;
	struct weston_mode mode;

	if (surface->configure != configure_presented_surface)
		return;
	scsurf = surface->configure_private;

	weston_surface_configure(surface, sx, sy, width, height);

	if (scsurf->method != WL_SYSTEM_COMPOSITOR_FULLSCREEN_METHOD_DRIVER)
		restore_output_mode(scsurf->output);

	surface_subsurfaces_boundingbox(surface, &surf_x, &surf_y,
	                                &surf_width, &surf_height);

	switch (scsurf->method) {
	case WL_SYSTEM_COMPOSITOR_FULLSCREEN_METHOD_DEFAULT:
		if (surface->buffer_ref.buffer)
			center_on_output(surface, scsurf->output);
		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE:
		/* 1:1 mapping between surface and output dimensions */
		if (scsurf->output->width == surf_width &&
		    scsurf->output->height == surf_height) {
			weston_surface_set_position(surface,
						    scsurf->output->x - surf_x,
						    scsurf->output->y - surf_y);
			break;
		}

		matrix = &scsurf->transform.matrix;
		weston_matrix_init(matrix);

		output_aspect = (float) scsurf->output->width /
			(float) scsurf->output->height;
		surface_aspect = (float) surface->geometry.width /
			(float) surface->geometry.height;
		if (output_aspect < surface_aspect)
			scale = (float) scsurf->output->width /
				(float) surf_width;
		else
			scale = (float) scsurf->output->height /
				(float) surf_height;

		weston_matrix_scale(matrix, scale, scale, 1);
		wl_list_remove(&scsurf->transform.link);
		wl_list_insert(&surface->geometry.transformation_list,
			       &scsurf->transform.link);
		x = scsurf->output->x + (scsurf->output->width - surf_width * scale) / 2 - surf_x;
		y = scsurf->output->y + (scsurf->output->height - surf_height * scale) / 2 - surf_y;
		weston_surface_set_position(surface, x, y);

		break;
		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER:
		mode.flags = 0;
		mode.width = surf_width * surface->buffer_scale;
		mode.height = surf_height * surface->buffer_scale;
		mode.refresh = scsurf->framerate;

		if (weston_output_switch_mode(scsurf->output, &mode,
		    surface->buffer_scale) == 0) {
			weston_surface_set_position(surface,
						    scsurf->output->x - surf_x,
						    scsurf->output->y - surf_y);
			weston_surface_configure(scsurf->black_surface,
				                 scsurf->output->x - surf_x,
				                 scsurf->output->y - surf_y,
						 scsurf->output->width,
						 scsurf->output->height);
			break;
		} else {
			restore_output_mode(scsurf->output);
			center_on_output(surface, scsurf->output);
		}
		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_FILL:
		center_on_output(surface, scsurf->output);
		break;
	default:
		break;
	}
}

static void
system_compositor_present_surface(struct wl_client *client,
				  struct wl_resource *resource,
				  struct wl_resource *surface_res,
				  uint32_t method, uint32_t framerate,
				  struct wl_resource *output_res)
{
	struct system_compositor *syscomp =
		wl_resource_get_user_data(resource);
	struct weston_output *output;
	struct weston_surface *surface = wl_resource_get_user_data(surface_res);
	struct sc_surface *scsurf;

	if (output_res) {
		output = wl_resource_get_user_data(output_res);
	} else if (! wl_list_empty(&syscomp->compositor->output_list)) {
		/* Just grab the first one */
		output = container_of(syscomp->compositor->output_list.next,
			    struct weston_output, link);
	} else {
		return;
	}

	scsurf = find_surface_for_output(syscomp, output);

	if (surface_res) {
		if (surface->configure_private) {
			wl_resource_post_error(surface_res,
					       WL_DISPLAY_ERROR_INVALID_OBJECT,
					       "surface already presented");
			return;
		}

		if (scsurf == NULL) {
			scsurf = malloc(sizeof *scsurf);
			memset(scsurf, 0, sizeof *scsurf);
			scsurf->black_surface =
				create_black_surface(syscomp->compositor,
						     scsurf,
						     output->x, output->y,
						     output->width,
						     output->height);
			if (scsurf->black_surface == NULL) {
				free(scsurf);
				return;
			}
			/* Put the black surface on the bottom */
			wl_list_insert(syscomp->layer.surface_list.prev,
				       &scsurf->black_surface->layer_link);

			wl_list_insert(&syscomp->surfaces_list, &scsurf->link);
			scsurf->output = output;
		}

		if (scsurf->surface && scsurf->surface != surface)
			weston_surface_unmap(scsurf->surface);

		surface->configure = configure_presented_surface;
		surface->configure_private = scsurf;

		/* Put this surface on the top */
		wl_list_insert(&syscomp->layer.surface_list,
			       &surface->layer_link);

		scsurf->surface = surface;
		scsurf->method = method;
		scsurf->framerate = framerate;

		// TODO: Call configure?
	} else if (scsurf) {
		weston_surface_destroy(scsurf->black_surface);
		weston_surface_unmap(scsurf->surface);
		wl_list_remove(&scsurf->link);
		free(scsurf);
	}

	weston_compositor_schedule_repaint(syscomp->compositor);
}

struct wl_system_compositor_interface system_compositor_implementation = {
	system_compositor_present_surface,
};

static void
bind_system_compositor(struct wl_client *client, void *data, uint32_t version,
		       uint32_t id)
{
	struct system_compositor *sysc = data;
	struct wl_resource *resource;

	if (sysc->client != NULL && sysc->client != client)
		return;
	sysc->client = client;

	resource = wl_resource_create(client, &wl_system_compositor_interface,
				      1, id);
	wl_resource_set_implementation(resource,
				       &system_compositor_implementation,
				       sysc, NULL);
}

WL_EXPORT int
module_init(struct weston_compositor *compositor,
	    int *argc, char *argv[])
{
	struct system_compositor *sysc;

	sysc = malloc(sizeof *sysc);
	if (sysc == NULL)
		return -1;

	memset(sysc, 0, sizeof *sysc);
	sysc->compositor = compositor;

	wl_list_init(&sysc->surfaces_list);

	wl_global_create(compositor->wl_display,
			 &wl_system_compositor_interface, 1, sysc,
			 bind_system_compositor);

	weston_layer_init(&sysc->layer, &compositor->cursor_layer.link);

	return 0;
}
