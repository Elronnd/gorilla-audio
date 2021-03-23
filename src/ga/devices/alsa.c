#include "gorilla/ga.h"
#include "gorilla/ga_internal.h"

#include <alsa/asoundlib.h>

static void *gaX_get_buffer_async(GaDevice*);
static ga_result gaX_queue_async(GaDevice*,void*);
static ga_result gaX_queue_pipe(GaDevice*,void*);

struct GaXDeviceImpl {
	snd_pcm_t *interface;
	snd_pcm_uframes_t last_offset;
};

static ga_result gaX_open(GaDevice *dev) {
	snd_pcm_format_t fmt;
	switch (dev->format.sample_fmt) {
		case GaSampleFormat_U8:  fmt = SND_PCM_FORMAT_U8;    break;
		case GaSampleFormat_S16: fmt = SND_PCM_FORMAT_S16;   break;
		case GaSampleFormat_S32: fmt = SND_PCM_FORMAT_S32;   break;
		case GaSampleFormat_F32: fmt = SND_PCM_FORMAT_FLOAT; break;
		default: return GA_ERR_MIS_PARAM;
	}

	switch (dev->class) {
		case GaDeviceClass_PushAsync:
		case GaDeviceClass_PushSync: break;
		case GaDeviceClass_Callback:
			dev->class = GaDeviceClass_PushSync;
	}

#define acheck(expr) do { if ((expr) < 0) { res = GA_ERR_SYS_LIB; ga_warn("alsa: '" #expr "' failed"); goto cleanup; } } while (0)
	ga_result res = GA_ERR_GENERIC;
	dev->impl = ga_alloc(sizeof(GaXDeviceImpl));
	if (!dev->impl) return GA_ERR_SYS_MEM;

	if (snd_pcm_open(&dev->impl->interface, "default", SND_PCM_STREAM_PLAYBACK, (dev->class == GaDeviceClass_PushAsync) * SND_PCM_NONBLOCK) < 0) {
		ga_free(dev->impl);
		return GA_ERR_SYS_LIB;
	}

	snd_pcm_hw_params_t *params = NULL;
#define alloca malloc
        snd_pcm_hw_params_alloca(&params);
#undef alloca
	if (!params) goto cleanup;
        acheck(snd_pcm_hw_params_any(dev->impl->interface, params));

        acheck(snd_pcm_hw_params_set_access(dev->impl->interface, params, (dev->class == GaDeviceClass_PushAsync) ? SND_PCM_ACCESS_MMAP_INTERLEAVED : SND_PCM_ACCESS_RW_INTERLEAVED));
        acheck(snd_pcm_hw_params_set_format(dev->impl->interface, params, fmt));

        acheck(snd_pcm_hw_params_set_channels(dev->impl->interface, params, dev->format.num_channels));
        acheck(snd_pcm_hw_params_set_buffer_size(dev->impl->interface, params, dev->num_frames * ga_format_frame_size(&dev->format)));
	// this can transparently change the frame rate from under the user
	// TODO: should we let them pass an option to error if they can't get exactly the desired frame rate?
	acheck(snd_pcm_hw_params_set_rate_near(dev->impl->interface, params, &dev->format.frame_rate, NULL));
	acheck(snd_pcm_hw_params(dev->impl->interface, params));

	if (dev->class == GaDeviceClass_PushAsync) {
		snd_pcm_avail(dev->impl->interface);
		const snd_pcm_channel_area_t *areas;
		snd_pcm_uframes_t frames = dev->num_frames;
		acheck(snd_pcm_mmap_begin(dev->impl->interface, &areas, &dev->impl->last_offset, &frames));
		acheck(snd_pcm_mmap_commit(dev->impl->interface, dev->impl->last_offset, frames));
		if (snd_pcm_state(dev->impl->interface) != SND_PCM_STATE_PREPARED) goto cleanup;
		acheck(snd_pcm_start(dev->impl->interface));
	}

	free(params);
	//todo latency

	if (dev->class == GaDeviceClass_PushAsync) {
		dev->procs.get_buffer = gaX_get_buffer_async;
		dev->procs.queue = gaX_queue_async;
	} else {
		dev->procs.queue = gaX_queue_pipe;
	}

	return GA_OK;

cleanup:
	free(params);
	snd_pcm_drain(dev->impl->interface);
	snd_pcm_close(dev->impl->interface);
	ga_free(dev->impl);
	dev->impl = NULL;
	return res;
}

static ga_result gaX_close(GaDevice *dev) {
	snd_pcm_drain(dev->impl->interface);
	snd_pcm_close(dev->impl->interface);
	ga_free(dev->impl);

	snd_config_update_free_global();
	// this just frees a global cache, that will be reinstated
	// transparently by alsa in the event that the library user creates and
	// then destroys multiple devices
	// but freeing it avoids false positives from valgrind/memtest
	return GA_OK;
}

static ga_result gaX_check(GaDevice *dev, u32 *num_frames) {
	//ga_info("state %d", snd_pcm_state(dev->impl->interface));
	snd_pcm_sframes_t avail = snd_pcm_avail(dev->impl->interface);
	if (avail < 0) {
		ga_warn("unable to query available buffers; code %li", avail);
		return GA_ERR_GENERIC; //negative is a 'code', but it's not specified what values it can take
	}
	*num_frames = avail / dev->num_frames;
	return GA_OK;
}

static void *gaX_get_buffer_async(GaDevice *dev) {
	// docs say of snd_pcm_mmap_begin:
	// 
	// > It is necessary to call the snd_pcm_avail_update() function
	// > directly before this call. Otherwise, this function can return a
	// > wrong count of available frames.
	// 
	// we shunt this responsibility onto the user; as documented, you must
	// call gaX_check before calling get_buffer

	const snd_pcm_channel_area_t *areas;
	snd_pcm_uframes_t frames = dev->num_frames;
	if (snd_pcm_mmap_begin(dev->impl->interface, &areas, &dev->impl->last_offset, &frames) != 0) return NULL;
	if (frames < dev->num_frames) {
		ga_warn("alsa shared memory buffer of insufficient size; this likely indicates an application bug (forgot to call ga_device_check())");
		goto fail;
	}

	if ((areas->first % 8) || (areas->step % 8)) {
		ga_err("WTF %u %u", areas->first, areas->step);
		goto fail;
	}

	if (areas->step != ga_format_frame_size(&dev->format) * 8) {
		ga_err("can't handle alsa buffer with step of '%u' bits (!= %u * 8)", areas->step, ga_format_sample_size(dev->format.sample_fmt));
	}

	if (!areas->addr) {
		ga_err("????");
		goto fail;
	}

	return (char*)areas->addr + areas->first/8 + dev->impl->last_offset*ga_format_frame_size(&dev->format);

fail:;
	snd_pcm_sframes_t t = snd_pcm_mmap_commit(dev->impl->interface, dev->impl->last_offset, 0);
	if (t != 0) ga_warn("while decommitting empty buffer: %li", t);
	return NULL;
}
static ga_result gaX_queue_async(GaDevice *dev, void *buf) {
	snd_pcm_sframes_t written = snd_pcm_mmap_commit(dev->impl->interface, dev->impl->last_offset, dev->num_frames);

	if (written > 0 && written < dev->num_frames) {
		ga_warn("only wrote %li/%u frames", written, dev->num_frames);
		return GA_ERR_SYS_RUN;
	}
	if (written < 0) {
		ga_warn("alsa async mmap returned %li", written);
		return GA_ERR_SYS_LIB;
	}
	return GA_OK;
}

static ga_result gaX_queue_pipe(GaDevice *dev, void *buf) {
	snd_pcm_sframes_t written = snd_pcm_writei(dev->impl->interface, buf, dev->num_frames);
	// TODO: handle the below (particularly run)
	if (written == -EBADFD) return GA_ERR_INTERNAL; // PCM is not in the right state (SND_PCM_STATE_PREPARED or SND_PCM_STATE_RUNNING) 
	if (written == -EPIPE) return GA_ERR_SYS_RUN; // underrun
	if (written == -ESTRPIPE) return GA_ERR_GENERIC; // a suspend event occurred (stream is suspended and waiting for an application recovery)

	if (written != dev->num_frames) return GA_ERR_GENERIC; // underrun/signal
	return GA_OK;
}

GaXDeviceProcs gaX_deviceprocs_ALSA = { .open=gaX_open, .check=gaX_check, .close=gaX_close };
