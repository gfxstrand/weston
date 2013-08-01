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
	struct weston_layer layer;
};

static void
system_compositor_present_surface(struct wl_client *client,
				  struct wl_resource *resource,
				  struct wl_resource *surface,
				  uint32_t method, uint32_t framerate,
				  struct wl_resource *output)
{
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

	if (sysc->client != client)
		return;

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

	wl_global_create(compositor->wl_display,
			 &wl_system_compositor_interface, 1, sysc,
			 bind_system_compositor);

	weston_layer_init(&sysc->layer, &compositor->cursor_layer.link);

	return 0;
}
