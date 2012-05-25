/*
 * Copyright © 2008-2011 Kristian Høgsberg
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>

#include "compositor.h"
#include "screenshooter-server-protocol.h"

struct screenshooter {
	struct wl_object base;
	struct weston_compositor *ec;
	struct wl_global *global;
	struct wl_client *client;
	struct weston_process process;
	struct wl_listener destroy_listener;
};

struct screenshooter_read_pixels {
	struct weston_read_pixels base;
	struct wl_buffer *buffer;
	struct wl_resource *resource;
};

static void
copy_bgra_yflip(uint8_t *dst, uint8_t *src, int height, int stride)
{
	uint8_t *end;

	end = dst + height * stride;
	while (dst < end) {
		memcpy(dst, src, stride);
		dst += stride;
		src -= stride;
	}
}

static void
copy_row_swap_RB(void *vdst, void *vsrc, int bytes)
{
	uint32_t *dst = vdst;
	uint32_t *src = vsrc;
	uint32_t *end = dst + bytes / 4;

	while (dst < end) {
		uint32_t v = *src++;
		/*                    A R G B */
		uint32_t tmp = v & 0xff00ff00;
		tmp |= (v >> 16) & 0x000000ff;
		tmp |= (v << 16) & 0x00ff0000;
		*dst++ = tmp;
	}
}

static void
copy_rgba_yflip(uint8_t *dst, uint8_t *src, int height, int stride)
{
	uint8_t *end;

	end = dst + height * stride;
	while (dst < end) {
		copy_row_swap_RB(dst, src, stride);
		dst += stride;
		src -= stride;
	}
}

static void
screenshooter_read_pixels_done(struct weston_read_pixels *base,
			       struct weston_output *output)
{
	struct screenshooter_read_pixels *r =
		(struct screenshooter_read_pixels *) base;
	int32_t stride;
	uint8_t *d, *s;

	stride = wl_shm_buffer_get_stride(r->buffer);

	d = wl_shm_buffer_get_data(r->buffer);
	s = r->base.data + stride * (r->buffer->height - 1);

	switch (output->compositor->read_format) {
	case GL_BGRA_EXT:
		copy_bgra_yflip(d, s, output->current->height, stride);
		break;
	case GL_RGBA:
		copy_rgba_yflip(d, s, output->current->height, stride);
		break;
	default:
		break;
	}

	wl_list_remove(&r->base.link);

	screenshooter_send_done(r->resource);
	free(r->base.data);
	free(r);

}

static void
screenshooter_shoot(struct wl_client *client,
		    struct wl_resource *resource,
		    struct wl_resource *output_resource,
		    struct wl_resource *buffer_resource)
{
	struct weston_output *output = output_resource->data;
	struct screenshooter_read_pixels *r;
	struct wl_buffer *buffer = buffer_resource->data;
	int32_t stride;

	if (!wl_buffer_is_shm(buffer))
		return;

	if (buffer->width < output->current->width ||
	    buffer->height < output->current->height)
		return;

	r = malloc(sizeof *r);
	if (r == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	r->base.x = 0;
	r->base.y = 0;
	r->base.width = output->current->width;
	r->base.height = output->current->height;
	r->base.done = screenshooter_read_pixels_done;
	r->buffer = buffer;
	r->resource = resource;
	stride = buffer->width * 4;
	r->base.data = malloc(stride * buffer->height);

	if (r->base.data == NULL) {
		free(r);
		wl_resource_post_no_memory(resource);
		return;
	}

	wl_list_insert(output->read_pixels_list.prev, &r->base.link);
	weston_compositor_schedule_repaint(output->compositor);
}

struct screenshooter_interface screenshooter_implementation = {
	screenshooter_shoot
};

static void
bind_shooter(struct wl_client *client,
	     void *data, uint32_t version, uint32_t id)
{
	struct screenshooter *shooter = data;
	struct wl_resource *resource;

	resource = wl_client_add_object(client, &screenshooter_interface,
			     &screenshooter_implementation, id, data);

	if (client != shooter->client) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "screenshooter failed: permission denied");
		wl_resource_destroy(resource);
	}
}

static void
screenshooter_sigchld(struct weston_process *process, int status)
{
	struct screenshooter *shooter =
		container_of(process, struct screenshooter, process);

	shooter->client = NULL;
}

static void
screenshooter_binding(struct wl_seat *seat, uint32_t time,
		 uint32_t key, uint32_t button, uint32_t axis,
		 int32_t state, void *data)
{
	struct screenshooter *shooter = data;
	const char *screenshooter_exe = LIBEXECDIR "/weston-screenshooter";

	if (!shooter->client)
		shooter->client = weston_client_launch(shooter->ec,
					&shooter->process,
					screenshooter_exe, screenshooter_sigchld);
}

struct weston_recorder {
	struct weston_output *output;
	uint32_t *frame, *rect;
	uint32_t total;
	int fd;
	struct wl_listener frame_listener;
	int count;
};

static uint32_t *
output_run(uint32_t *p, uint32_t delta, int run)
{
	int i;

	while (run > 0) {
		if (run <= 0xe0) {
			*p++ = delta | ((run - 1) << 24);
			break;
		}

		i = 24 - __builtin_clz(run);
		*p++ = delta | ((i + 0xe0) << 24);
		run -= 1 << (7 + i);
	}

	return p;
}

static void
weston_recorder_frame_notify(struct wl_listener *listener, void *data)
{
	struct weston_recorder *recorder =
		container_of(listener, struct weston_recorder, frame_listener);
	struct weston_output *output = recorder->output;
	uint32_t msecs = * (uint32_t *) data;
	pixman_box32_t *r;
	pixman_region32_t damage;
	int i, j, k, n, width, height, run, stride;
	uint32_t delta, prev, *d, *s, *p, next;
	struct {
		uint32_t msecs;
		uint32_t nrects;
	} header;
	struct iovec v[2];

	pixman_region32_init(&damage);
	pixman_region32_intersect(&damage, &recorder->output->region,
				  &recorder->output->previous_damage);

	r = pixman_region32_rectangles(&damage, &n);
	if (n == 0)
		return;
	if (recorder->count++ == 0)
		/* The first callback gives us the frame immediately
		 * before the weston_output_damage() call, and
		 * typically doesn't give us a full frame of damage.*/
		return;

	header.msecs = msecs;
	header.nrects = n;
	v[0].iov_base = &header;
	v[0].iov_len = sizeof header;
	v[1].iov_base = r;
	v[1].iov_len = n * sizeof *r;
	recorder->total += writev(recorder->fd, v, 2);
	stride = output->current->width;

	for (i = 0; i < n; i++) {
		width = r[i].x2 - r[i].x1;
		height = r[i].y2 - r[i].y1;
		glReadPixels(r[i].x1, output->current->height - r[i].y2,
			     width, height,
			     output->compositor->read_format,
			     GL_UNSIGNED_BYTE, recorder->rect);

		s = recorder->rect;
		p = recorder->rect;
		run = prev = 0; /* quiet gcc */
		for (j = 0; j < height; j++) {
			d = recorder->frame +
				stride * (r[i].y2 - j - 1) + r[i].x1;
			for (k = 0; k < width; k++) {
				next = *s++;
				delta = (next - *d) & 0x00ffffff;
				*d++ = next;
				if (run == 0 || delta == prev) {
					run++;
				} else {
					p = output_run(p, prev, run);
					run = 1;
					prev = delta;
				}
			}
		}

		p = output_run(p, prev, run);

		recorder->total += write(recorder->fd,
					 recorder->rect,
					 (p - recorder->rect) * 4);

#if 0
		fprintf(stderr,
			"%dx%d at %d,%d rle from %d to %d bytes (%f) total %dM\n",
			width, height, r[i].x1, r[i].y1,
			width * height * 4, (int) (p - rect) * 4,
			(float) (p - rect) / (width * height),
			total / 1024 / 1024);
#endif
	}

	pixman_region32_fini(&damage);
}

#define WCAP_HEADER_MAGIC	0x57434150
#define WCAP_FORMAT_XRGB8888	0x34325258
#define WCAP_FORMAT_XBGR8888	0x34324258
#define WCAP_FORMAT_RGBX8888	0x34325852
#define WCAP_FORMAT_BGRX8888	0x34325842

static void
weston_recorder_create(struct weston_output *output, const char *filename)
{
	struct weston_recorder *recorder;
	int stride, size;
	struct { uint32_t magic, format, width, height; } header;

	recorder = malloc(sizeof *recorder);
	recorder->output = output;

	stride = output->current->width;
	size = stride * 4 * output->current->height;
	recorder->frame = malloc(size);
	recorder->rect = malloc(size);
	recorder->total = 0;
	recorder->count = 0;
	memset(recorder->frame, 0, size);

	recorder->fd = open(filename,
			    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	header.magic = WCAP_HEADER_MAGIC;

	switch (output->compositor->read_format) {
	case GL_BGRA_EXT:
		header.format = WCAP_FORMAT_XRGB8888;
		break;
	case GL_RGBA:
		header.format = WCAP_FORMAT_XBGR8888;
		break;
	}

	header.width = output->current->width;
	header.height = output->current->height;
	recorder->total += write(recorder->fd, &header, sizeof header);

	recorder->frame_listener.notify = weston_recorder_frame_notify;
	wl_signal_add(&output->frame_signal, &recorder->frame_listener);
	weston_output_damage(output);
}

static void
weston_recorder_destroy(struct weston_recorder *recorder)
{
	wl_list_remove(&recorder->frame_listener.link);
	close(recorder->fd);
	free(recorder->frame);
	free(recorder->rect);
	free(recorder);
}

static void
recorder_binding(struct wl_seat *seat, uint32_t time,
		 uint32_t key, uint32_t button, uint32_t axis,
		 int32_t state, void *data)
{
	struct weston_seat *ws = (struct weston_seat *) seat;
	struct weston_compositor *ec = ws->compositor;
	struct weston_output *output =
		container_of(ec->output_list.next,
			     struct weston_output, link);
	struct wl_listener *listener;
	struct weston_recorder *recorder;
	static const char filename[] = "capture.wcap";

	listener = wl_signal_get(&output->frame_signal,
				 weston_recorder_frame_notify);
	if (listener) {
		recorder = container_of(listener, struct weston_recorder,
					frame_listener);

		fprintf(stderr,
			"stopping recorder, total file size %dM, %d frames\n",
			recorder->total / (1024 * 1024), recorder->count);

		weston_recorder_destroy(recorder);
	} else {
		fprintf(stderr, "starting recorder, file %s\n", filename);
		weston_recorder_create(output, filename);
	}
}

static void
screenshooter_destroy(struct wl_listener *listener, void *data)
{
	struct screenshooter *shooter =
		container_of(listener, struct screenshooter, destroy_listener);

	wl_display_remove_global(shooter->ec->wl_display, shooter->global);
	free(shooter);
}

void
screenshooter_create(struct weston_compositor *ec)
{
	struct screenshooter *shooter;

	shooter = malloc(sizeof *shooter);
	if (shooter == NULL)
		return;

	shooter->base.interface = &screenshooter_interface;
	shooter->base.implementation =
		(void(**)(void)) &screenshooter_implementation;
	shooter->ec = ec;
	shooter->client = NULL;

	shooter->global = wl_display_add_global(ec->wl_display,
						&screenshooter_interface,
						shooter, bind_shooter);
	weston_compositor_add_binding(ec, KEY_S, 0, 0, MODIFIER_SUPER,
					screenshooter_binding, shooter);
	weston_compositor_add_binding(ec, KEY_R, 0, 0, MODIFIER_SUPER,
					recorder_binding, shooter);

	shooter->destroy_listener.notify = screenshooter_destroy;
	wl_signal_add(&ec->destroy_signal, &shooter->destroy_listener);
}
