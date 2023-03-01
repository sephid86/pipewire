/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

static void process_audio_playback(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t wanted, timestamp, target_buffer, stride, maxsize;
	int32_t avail;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	stride = impl->stride;

	maxsize = d[0].maxsize / stride;
	wanted = buf->requested ? SPA_MIN(buf->requested, maxsize) : maxsize;

	if (impl->io_position && impl->direct_timestamp) {
		/* in direct mode, read directly from the timestamp index,
		 * because sender and receiver are in sync, this would keep
		 * target_buffer of samples available. */
		spa_ringbuffer_read_update(&impl->ring,
				impl->io_position->clock.position);
	}
	avail = spa_ringbuffer_get_read_index(&impl->ring, &timestamp);

	target_buffer = impl->target_buffer;

	if (avail < (int32_t)wanted) {
		enum spa_log_level level;
		memset(d[0].data, 0, wanted * stride);
		if (impl->have_sync) {
			impl->have_sync = false;
			level = SPA_LOG_LEVEL_WARN;
		} else {
			level = SPA_LOG_LEVEL_DEBUG;
		}
		pw_log(level, "underrun %d/%u < %u",
					avail, target_buffer, wanted);
	} else {
		float error, corr;
		if (impl->first) {
			if ((uint32_t)avail > target_buffer) {
				uint32_t skip = avail - target_buffer;
				pw_log_debug("first: avail:%d skip:%u target:%u",
							avail, skip, target_buffer);
				timestamp += skip;
				avail = target_buffer;
			}
			impl->first = false;
		} else if (avail > (int32_t)SPA_MIN(target_buffer * 8, BUFFER_SIZE / stride)) {
			pw_log_warn("overrun %u > %u", avail, target_buffer * 8);
			timestamp += avail - target_buffer;
			avail = target_buffer;
		}
		if (!impl->direct_timestamp) {
			/* when not using direct timestamp and clocks are not
			 * in sync, try to adjust our playback rate to keep the
			 * requested target_buffer bytes in the ringbuffer */
			error = (float)target_buffer - (float)avail;
			error = SPA_CLAMP(error, -impl->max_error, impl->max_error);

			corr = spa_dll_update(&impl->dll, error);

			pw_log_debug("avail:%u target:%u error:%f corr:%f", avail,
					target_buffer, error, corr);

			if (impl->io_rate_match) {
				SPA_FLAG_SET(impl->io_rate_match->flags,
						SPA_IO_RATE_MATCH_FLAG_ACTIVE);
				impl->io_rate_match->rate = 1.0f / corr;
			}
		}
		spa_ringbuffer_read_data(&impl->ring,
				impl->buffer,
				BUFFER_SIZE,
				(timestamp * stride) & BUFFER_MASK,
				d[0].data, wanted * stride);

		timestamp += wanted;
		spa_ringbuffer_read_update(&impl->ring, timestamp);
	}
	d[0].chunk->size = wanted * stride;
	d[0].chunk->stride = stride;
	d[0].chunk->offset = 0;
	buf->size = wanted;

	pw_stream_queue_buffer(impl->stream, buf);
}

static void receive_rtp_audio(struct impl *impl, uint8_t *buffer, ssize_t len)
{
	struct rtp_header *hdr;
	ssize_t hlen, plen;
	uint16_t seq;
	uint32_t timestamp, samples, write, expected_write;
	uint32_t stride = impl->stride;
	int32_t filled;

	if (len < 12)
		goto short_packet;

	hdr = (struct rtp_header*)buffer;
	if (hdr->v != 2)
		goto invalid_version;


	hlen = 12 + hdr->cc * 4;
	if (hlen > len)
		goto invalid_len;

	if (impl->have_ssrc && impl->ssrc != hdr->ssrc)
		goto unexpected_ssrc;
	impl->ssrc = hdr->ssrc;
	impl->have_ssrc = true;

	seq = ntohs(hdr->sequence_number);
	if (impl->have_seq && impl->seq != seq) {
		pw_log_info("unexpected seq (%d != %d) SSRC:%u",
				seq, impl->seq, hdr->ssrc);
		impl->have_sync = false;
	}
	impl->seq = seq + 1;
	impl->have_seq = true;

	timestamp = ntohl(hdr->timestamp) - impl->ts_offset;

	impl->receiving = true;

	plen = len - hlen;
	samples = plen / stride;

	filled = spa_ringbuffer_get_write_index(&impl->ring, &expected_write);

	/* we always write to timestamp + delay */
	write = timestamp + impl->target_buffer;

	if (!impl->have_sync) {
		pw_log_info("sync to timestamp:%u seq:%u ts_offset:%u SSRC:%u direct:%d",
				write, impl->seq-1, impl->ts_offset, impl->ssrc,
				impl->direct_timestamp);

		/* we read from timestamp, keeping target_buffer of data
		 * in the ringbuffer. */
		impl->ring.readindex = timestamp;
		impl->ring.writeindex = write;
		filled = impl->target_buffer;

		spa_dll_init(&impl->dll);
		spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MIN, 128, impl->rate);
		memset(impl->buffer, 0, BUFFER_SIZE);
		impl->have_sync = true;
	} else if (expected_write != write) {
		pw_log_debug("unexpected write (%u != %u)",
				write, expected_write);
	}

	if (filled + samples > BUFFER_SIZE / stride) {
		pw_log_debug("capture overrun %u + %u > %u", filled, samples,
				BUFFER_SIZE / stride);
		impl->have_sync = false;
	} else {
		pw_log_debug("got samples:%u", samples);
		spa_ringbuffer_write_data(&impl->ring,
				impl->buffer,
				BUFFER_SIZE,
				(write * stride) & BUFFER_MASK,
				&buffer[hlen], (samples * stride));
		write += samples;
		spa_ringbuffer_write_update(&impl->ring, write);
	}
	return;

short_packet:
	pw_log_warn("short packet received");
	return;
invalid_version:
	pw_log_warn("invalid RTP version");
	spa_debug_mem(0, buffer, len);
	return;
invalid_len:
	pw_log_warn("invalid RTP length");
	return;
unexpected_ssrc:
	pw_log_warn("unexpected SSRC (expected %u != %u)",
		impl->ssrc, hdr->ssrc);
	return;
}

static inline void
set_iovec(struct spa_ringbuffer *rbuf, void *buffer, uint32_t size,
		uint32_t offset, struct iovec *iov, uint32_t len)
{
	iov[0].iov_len = SPA_MIN(len, size - offset);
	iov[0].iov_base = SPA_PTROFF(buffer, offset, void);
	iov[1].iov_len = len - iov[0].iov_len;
	iov[1].iov_base = buffer;
}

static void flush_audio_packets(struct impl *impl)
{
	int32_t avail;
	uint32_t stride, timestamp;
	struct iovec iov[3];
	struct rtp_header header;
	int32_t tosend;

	avail = spa_ringbuffer_get_read_index(&impl->ring, &timestamp);
	tosend = impl->psamples;

	if (avail < tosend)
		return;

	stride = impl->stride;

	spa_zero(header);
	header.v = 2;
	header.pt = impl->payload;
	header.ssrc = htonl(impl->ssrc);

	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);

	while (avail >= tosend) {
		header.sequence_number = htons(impl->seq);
		header.timestamp = htonl(impl->ts_offset + timestamp);

		set_iovec(&impl->ring,
			impl->buffer, BUFFER_SIZE,
			(timestamp * stride) & BUFFER_MASK,
			&iov[1], tosend * stride);

		pw_log_trace("sending %d timestamp:%d", tosend, timestamp);

		rtp_stream_emit_send_packet(impl, iov, 3);

		impl->seq++;
		timestamp += tosend;
		avail -= tosend;
	}
	spa_ringbuffer_read_update(&impl->ring, timestamp);
}

static void process_audio_capture(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t offs, size, timestamp, expected_timestamp, stride;
	int32_t filled, wanted;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	offs = SPA_MIN(d[0].chunk->offset, d[0].maxsize);
	size = SPA_MIN(d[0].chunk->size, d[0].maxsize - offs);
	stride = impl->stride;
	wanted = size / stride;

	filled = spa_ringbuffer_get_write_index(&impl->ring, &expected_timestamp);
	if (SPA_LIKELY(impl->io_position))
		timestamp = impl->io_position->clock.position;
	else
		timestamp = expected_timestamp;

	if (impl->have_sync) {
		if (expected_timestamp != timestamp) {
			pw_log_warn("expected %u != timestamp %u", expected_timestamp, timestamp);
			impl->have_sync = false;
		} else if (filled + wanted > (int32_t)(BUFFER_SIZE / stride)) {
			pw_log_warn("overrun %u + %u > %u", filled, wanted, BUFFER_SIZE / stride);
			impl->have_sync = false;
		}
	}
	if (!impl->have_sync) {
		pw_log_info("sync to timestamp:%u seq:%u ts_offset:%u SSRC:%u",
				timestamp, impl->seq, impl->ts_offset, impl->ssrc);
		impl->ring.readindex = impl->ring.writeindex = timestamp;
		memset(impl->buffer, 0, BUFFER_SIZE);
		impl->have_sync = true;
	}

	spa_ringbuffer_write_data(&impl->ring,
			impl->buffer,
			BUFFER_SIZE,
			(timestamp * stride) & BUFFER_MASK,
			SPA_PTROFF(d[0].data, offs, void), wanted * stride);
	timestamp += wanted;
	spa_ringbuffer_write_update(&impl->ring, timestamp);

	pw_stream_queue_buffer(impl->stream, buf);

	flush_audio_packets(impl);
}