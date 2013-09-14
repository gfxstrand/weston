/*
 * Copyright © 2011 Intel Corporation
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
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <unistd.h>
#include <fcntl.h>

#include "compositor.h"

WL_EXPORT void
weston_spring_init(struct weston_spring *spring,
		   double k, double current, double target)
{
	spring->k = k;
	spring->friction = 400.0;
	spring->current = current;
	spring->previous = current;
	spring->target = target;
	spring->clip = WESTON_SPRING_OVERSHOOT;
	spring->min = 0.0;
	spring->max = 1.0;
}

WL_EXPORT void
weston_spring_update(struct weston_spring *spring, uint32_t msec)
{
	double force, v, current, step;

	/* Limit the number of executions of the loop below by ensuring that
	 * the timestamp for last update of the spring is no more than 1s ago.
	 * This handles the case where time moves backwards or forwards in
	 * large jumps.
	 */
	if (msec - spring->timestamp > 1000) {
		weston_log("unexpectedly large timestamp jump (from %u to %u)\n",
			   spring->timestamp, msec);
		spring->timestamp = msec - 1000;
	}

	step = 0.01;
	while (4 < msec - spring->timestamp) {
		current = spring->current;
		v = current - spring->previous;
		force = spring->k * (spring->target - current) / 10.0 +
			(spring->previous - current) - v * spring->friction;

		spring->current =
			current + (current - spring->previous) +
			force * step * step;
		spring->previous = current;

		switch (spring->clip) {
		case WESTON_SPRING_OVERSHOOT:
			break;

		case WESTON_SPRING_CLAMP:
			if (spring->current > spring->max) {
				spring->current = spring->max;
				spring->previous = spring->max;
			} else if (spring->current < 0.0) {
				spring->current = spring->min;
				spring->previous = spring->min;
			}
			break;

		case WESTON_SPRING_BOUNCE:
			if (spring->current > spring->max) {
				spring->current =
					2 * spring->max - spring->current;
				spring->previous =
					2 * spring->max - spring->previous;
			} else if (spring->current < spring->min) {
				spring->current =
					2 * spring->min - spring->current;
				spring->previous =
					2 * spring->min - spring->previous;
			}
			break;
		}

		spring->timestamp += 4;
	}
}

WL_EXPORT int
weston_spring_done(struct weston_spring *spring)
{
	return fabs(spring->previous - spring->target) < 0.002 &&
		fabs(spring->current - spring->target) < 0.002;
}

typedef	void (*weston_view_animation_frame_func_t)(struct weston_view_animation *animation);

struct weston_view_animation {
	struct weston_view *view;
	struct weston_animation animation;
	struct weston_spring spring;
	struct weston_transform transform;
	struct wl_listener listener;
	float start, stop;
	weston_view_animation_frame_func_t frame;
	weston_view_animation_done_func_t done;
	void *data;
};

static void
weston_view_animation_destroy(struct weston_view_animation *animation)
{
	wl_list_remove(&animation->animation.link);
	wl_list_remove(&animation->listener.link);
	wl_list_remove(&animation->transform.link);
	weston_view_geometry_dirty(animation->view);
	if (animation->done)
		animation->done(animation, animation->data);
	free(animation);
}

static void
handle_animation_view_destroy(struct wl_listener *listener, void *data)
{
	struct weston_view_animation *animation =
		container_of(listener,
			     struct weston_view_animation, listener);

	weston_view_animation_destroy(animation);
}

static void
weston_view_animation_frame(struct weston_animation *base,
			    struct weston_output *output, uint32_t msecs)
{
	struct weston_view_animation *animation =
		container_of(base,
			     struct weston_view_animation, animation);

	if (base->frame_counter <= 1)
		animation->spring.timestamp = msecs;

	weston_spring_update(&animation->spring, msecs);

	if (weston_spring_done(&animation->spring)) {
		weston_view_animation_destroy(animation);
		return;
	}

	if (animation->frame)
		animation->frame(animation);

	weston_view_geometry_dirty(animation->view);
	weston_view_schedule_repaint(animation->view);
}

static struct weston_view_animation *
weston_view_animation_run(struct weston_view *view,
			  float start, float stop,
			  weston_view_animation_frame_func_t frame,
			  weston_view_animation_done_func_t done,
			  void *data)
{
	struct weston_view_animation *animation;

	animation = malloc(sizeof *animation);
	if (!animation)
		return NULL;

	animation->view = view;
	animation->frame = frame;
	animation->done = done;
	animation->data = data;
	animation->start = start;
	animation->stop = stop;
	weston_matrix_init(&animation->transform.matrix);
	wl_list_insert(&view->geometry.transformation_list,
		       &animation->transform.link);
	weston_spring_init(&animation->spring, 200.0, 0.0, 1.0);
	animation->spring.friction = 700;
	animation->animation.frame_counter = 0;
	animation->animation.frame = weston_view_animation_frame;
	weston_view_animation_frame(&animation->animation, NULL, 0);

	animation->listener.notify = handle_animation_view_destroy;
	wl_signal_add(&view->destroy_signal, &animation->listener);

	wl_list_insert(&view->output->animation_list,
		       &animation->animation.link);

	return animation;
}

static void
zoom_frame(struct weston_view_animation *animation)
{
	struct weston_view *es = animation->view;
	float scale;

	scale = animation->start +
		(animation->stop - animation->start) *
		animation->spring.current;
	weston_matrix_init(&animation->transform.matrix);
	weston_matrix_translate(&animation->transform.matrix,
				-0.5f * es->geometry.width,
				-0.5f * es->geometry.height, 0);
	weston_matrix_scale(&animation->transform.matrix, scale, scale, scale);
	weston_matrix_translate(&animation->transform.matrix,
				0.5f * es->geometry.width,
				0.5f * es->geometry.height, 0);

	es->alpha = animation->spring.current;
	if (es->alpha > 1.0)
		es->alpha = 1.0;
}

WL_EXPORT struct weston_view_animation *
weston_zoom_run(struct weston_view *view, float start, float stop,
		weston_view_animation_done_func_t done, void *data)
{
	struct weston_view_animation *zoom;

	zoom = weston_view_animation_run(view, start, stop,
					 zoom_frame, done, data);

	weston_spring_init(&zoom->spring, 300.0, start, stop);
	zoom->spring.friction = 1400;
	zoom->spring.previous = start - (stop - start) * 0.03;

	return zoom;
}

static void
fade_frame(struct weston_view_animation *animation)
{
	if (animation->spring.current > 0.999)
		animation->view->alpha = 1;
	else if (animation->spring.current < 0.001 )
		animation->view->alpha = 0;
	else
		animation->view->alpha = animation->spring.current;
}

WL_EXPORT struct weston_view_animation *
weston_fade_run(struct weston_view *view,
		float start, float end, float k,
		weston_view_animation_done_func_t done, void *data)
{
	struct weston_view_animation *fade;

	fade = weston_view_animation_run(view, 0, 0,
					 fade_frame, done, data);

	weston_spring_init(&fade->spring, k, start, end);

	fade->spring.friction = 1400;
	fade->spring.previous = -(end - start) * 0.03;

	view->alpha = start;

	return fade;
}

WL_EXPORT void
weston_fade_update(struct weston_view_animation *fade, float target)
{
	fade->spring.target = target;
}

static void
slide_frame(struct weston_view_animation *animation)
{
	float scale;

	scale = animation->start +
		(animation->stop - animation->start) *
		animation->spring.current;
	weston_matrix_init(&animation->transform.matrix);
	weston_matrix_translate(&animation->transform.matrix, 0, scale, 0);
}

WL_EXPORT struct weston_view_animation *
weston_slide_run(struct weston_view *view, float start, float stop,
		 weston_view_animation_done_func_t done, void *data)
{
	struct weston_view_animation *animation;

	animation = weston_view_animation_run(view, start, stop,
					      slide_frame, done, data);
	if (!animation)
		return NULL;

	animation->spring.friction = 600;
	animation->spring.k = 400;
	animation->spring.clip = WESTON_SPRING_BOUNCE;

	return animation;
}
