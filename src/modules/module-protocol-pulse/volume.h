/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
 * Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef PULSE_SERVER_VOLUME_H
#define PULSE_SERVER_VOLUME_H

#include "internal.h"

struct volume_info {
	struct volume volume;
	struct channel_map map;
	bool mute;
	float level;
	float base;
	uint32_t steps;
#define VOLUME_HW_VOLUME	(1<<0)
#define VOLUME_HW_MUTE		(1<<1)
	uint32_t flags;
};

#define VOLUME_INFO_INIT	(struct volume_info) {		\
					.volume = VOLUME_INIT,	\
					.mute = false,		\
					.level = 1.0,		\
					.base = 1.0,		\
					.steps = 256,		\
				}

#endif
