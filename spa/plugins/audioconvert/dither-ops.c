/* Spa
 *
 * Copyright © 2022 Wim Taymans
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

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#include <spa/param/audio/format-utils.h>
#include <spa/support/cpu.h>
#include <spa/support/log.h>
#include <spa/utils/defs.h>

#include "dither-ops.h"

#define DITHER_SIZE	(1<<10)

typedef void (*dither_func_t) (struct dither *d, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples);

static const struct dither_info {
	dither_func_t process;
	uint32_t cpu_flags;
} dither_table[] =
{
#if defined (HAVE_SSE2)
	{ dither_f32_sse2, 0 },
#endif
	{ dither_f32_c, 0 },
};

#define MATCH_CPU_FLAGS(a,b)	((a) == 0 || ((a) & (b)) == a)

static const struct dither_info *find_dither_info(uint32_t cpu_flags)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(dither_table); i++) {
		if (!MATCH_CPU_FLAGS(dither_table[i].cpu_flags, cpu_flags))
			continue;
		return &dither_table[i];
	}
	return NULL;
}

static void impl_dither_free(struct dither *d)
{
	d->process = NULL;
	free(d->dither);
	d->dither = NULL;
}

int dither_init(struct dither *d)
{
	const struct dither_info *info;
	size_t i;

	info = find_dither_info(d->cpu_flags);
	if (info == NULL)
		return -ENOTSUP;

	if (d->intensity >= 32)
		return -EINVAL;

	d->scale = 1.0f / (1ULL << (31 + d->intensity));

	d->dither_size = DITHER_SIZE;
	d->dither = calloc(d->dither_size + 8, sizeof(float));
	if (d->dither == NULL)
		return -errno;

	for (i = 0; i < SPA_N_ELEMENTS(d->random); i++)
		d->random[i] = random();


	d->free = impl_dither_free;
	d->process = info->process;
	return 0;
}
