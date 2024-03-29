/*
 * androidsound_opensles.cpp -Android Media plugin for Linphone, based on OpenSLES APIs.
 *
 * Copyright (C) 2014  Belledonne Communications, Grenoble, France
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <mediastreamer2/msfilter.h>
#include <mediastreamer2/msticker.h>
#include <mediastreamer2/mssndcard.h>

#include <sys/types.h>
#include <string.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>
#include <jni.h>
#include <dlfcn.h>
#include "audiofilters/devices.h"

namespace fake_opensles {
	SLInterfaceID SLW_IID_ENGINE = NULL;
	SLInterfaceID SLW_IID_ANDROIDSIMPLEBUFFERQUEUE = NULL;
	SLInterfaceID SLW_IID_ANDROIDCONFIGURATION = NULL;
	SLInterfaceID SLW_IID_RECORD = NULL;
	SLInterfaceID SLW_IID_VOLUME = NULL;
	SLInterfaceID SLW_IID_PLAY = NULL;

	typedef SLresult (*OpenSLESConstructor)(
		SLObjectItf*,
		SLuint32,
		const SLEngineOption*,
		SLuint32,
		const SLInterfaceID*,
		const SLboolean*
	);

	OpenSLESConstructor slwCreateEngine = NULL;

	int findSymbol(void *handle, SLInterfaceID &dest, const char *name) {
		SLInterfaceID *sym = (SLInterfaceID *) dlsym(handle, name);
		const char *error = dlerror();
		if (sym == NULL || error) {
			ms_error("Couldn't find %s symbol : %s", name, error);
			return 1;
		}
		dest = *sym;
		return 0;
	}

	int initOpenSLES() {
		int result = 0;
		void *handle;

		if ((handle = dlopen("libOpenSLES.so", RTLD_NOW)) == NULL){
			ms_warning("Fail to load libOpenSLES : %s", dlerror());
			result = -1;
		} else {
			dlerror(); // Clear previous message if present

			result += findSymbol(handle, SLW_IID_ENGINE, "SL_IID_ENGINE");
			result += findSymbol(handle, SLW_IID_ANDROIDSIMPLEBUFFERQUEUE, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
			result += findSymbol(handle, SLW_IID_ANDROIDCONFIGURATION, "SL_IID_ANDROIDCONFIGURATION");
			result += findSymbol(handle, SLW_IID_RECORD, "SL_IID_RECORD");
			result += findSymbol(handle, SLW_IID_VOLUME, "SL_IID_VOLUME");
			result += findSymbol(handle, SLW_IID_PLAY, "SL_IID_PLAY");

			slwCreateEngine = (OpenSLESConstructor) dlsym(handle, "slCreateEngine");
			if (slwCreateEngine == NULL) {
				result += 1;
				ms_error("Couldn't find slCreateEngine symbol");
			}
		}
		return result;
	}
}

#define NATIVE_USE_HARDWARE_RATE 1

static int sdk_version = 0;

static const int flowControlIntervalMs = 1000;
static const int flowControlThresholdMs = 40;

static int DeviceFavoriteSampleRate = 44100;
static int DeviceFavoriteBufferSize = 64;

#ifdef __cplusplus
extern "C" {
#endif
JNIEXPORT void JNICALL Java_org_linphone_mediastream_MediastreamerAndroidContext_setDeviceFavoriteSampleRate(JNIEnv* env, jclass thiz, jint samplerate) {
	DeviceFavoriteSampleRate = (int)samplerate;
}

JNIEXPORT void JNICALL Java_org_linphone_mediastream_MediastreamerAndroidContext_setDeviceFavoriteBufferSize(JNIEnv* env, jclass thiz, jint buffersize) {
	DeviceFavoriteBufferSize = (int)buffersize;
}
#ifdef __cplusplus
}
#endif 

using namespace fake_opensles;

static MSSndCard *android_snd_card_new(void);
static MSFilter *ms_android_snd_read_new(void);
static MSFilter *ms_android_snd_write_new(void);

struct OpenSLESContext {
	OpenSLESContext() {
		samplerate = DeviceFavoriteSampleRate;
		nchannels = 1;
		builtin_aec = false;
	}

	int samplerate;
	int nchannels;
	bool builtin_aec;

	SLObjectItf engineObject;
	SLEngineItf engineEngine;
};

struct OpenSLESOutputContext {
	OpenSLESOutputContext() {
		streamType = SL_ANDROID_STREAM_VOICE;
		nbufs = 0;
		outBufSize = DeviceFavoriteBufferSize;
		ms_bufferizer_init(&buffer);
		ms_mutex_init(&mutex,NULL);

		currentBuffer = 0;
		playBuffer[0] = (uint8_t *) calloc(outBufSize, sizeof(uint8_t));
		playBuffer[1] = (uint8_t *) calloc(outBufSize, sizeof(uint8_t));
	}

	~OpenSLESOutputContext() {
		free(playBuffer[0]);
		free(playBuffer[1]);
		ms_bufferizer_uninit(&buffer);
		ms_mutex_destroy(&mutex);
	}

	void setContext(OpenSLESContext *context) {
		opensles_context = context;
	}

	OpenSLESContext *opensles_context;

	SLObjectItf outputMixObject;
	SLObjectItf playerObject;
	SLPlayItf playerPlay;
	SLAndroidSimpleBufferQueueItf playerBufferQueue;
	SLAndroidConfigurationItf playerConfig;
	SLint32 streamType;

	MSBufferizer buffer;
	int nbufs;
	ms_mutex_t mutex;
	uint64_t flowControlStart;
	int minBufferFilling;

	uint8_t *playBuffer[2];
	int outBufSize;
	int currentBuffer;
};

struct OpenSLESInputContext {
	OpenSLESInputContext() {
		streamType = SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION;
		inBufSize = DeviceFavoriteBufferSize;
		qinit(&q);
		ms_mutex_init(&mutex,NULL);
		mTickerSynchronizer = NULL;
		aec = NULL;
		
		currentBuffer = 0;
		recBuffer[0] = (uint8_t *) calloc(inBufSize, sizeof(uint8_t));
		recBuffer[1] = (uint8_t *) calloc(inBufSize, sizeof(uint8_t));
	}

	~OpenSLESInputContext() {
		free(recBuffer[0]);
		free(recBuffer[1]);
		flushq(&q,0);
		ms_mutex_destroy(&mutex);
	}

	void setContext(OpenSLESContext *context) {
		opensles_context = context;
	}

	OpenSLESContext *opensles_context;

	SLObjectItf recorderObject;
	SLRecordItf recorderRecord;
	SLAndroidSimpleBufferQueueItf recorderBufferQueue;
	SLAndroidConfigurationItf recorderConfig;
	SLint32 streamType;

	queue_t q;
	ms_mutex_t mutex;
	MSTickerSynchronizer *mTickerSynchronizer;
	MSFilter *mFilter;
	int64_t read_samples;
	jobject aec;

	uint8_t *recBuffer[2];
	int inBufSize;
	int currentBuffer;
};

static SLuint32 convertSamplerate(int samplerate)
{
    switch(samplerate) {
    case 8000:
        return SL_SAMPLINGRATE_8;
        break;
    case 16000:
        return SL_SAMPLINGRATE_16;
        break;
    case 22050:
        return SL_SAMPLINGRATE_22_05;
        break;
    case 32000:
        return SL_SAMPLINGRATE_32;
        break;
    case 44100:
        return SL_SAMPLINGRATE_44_1;
        break;
    case 48000:
        return SL_SAMPLINGRATE_48;
        break;
    default:
        return -1;
    }
}

static void android_snd_card_detect(MSSndCardManager *m) {
	/* Get Android SDK version. */
	JNIEnv *jni_env = ms_get_jni_env();
	jclass version_class = jni_env->FindClass("android/os/Build$VERSION");
	jfieldID fid = jni_env->GetStaticFieldID(version_class, "SDK_INT", "I");
	sdk_version = jni_env->GetStaticIntField(version_class, fid);
	ms_message("SDK version [%i] detected", sdk_version);
	jni_env->DeleteLocalRef(version_class);
	
	if (sdk_version >= 19) { // Use only if Android OS >= KIT_KAT (4.4)
		if (initOpenSLES() == 0) { // Try to dlopen libOpenSLES
			ms_message("Android version is %i, libOpenSLES correctly loaded, creating OpenSLES MS soundcard", sdk_version);
			MSSndCard *card = android_snd_card_new();
			ms_snd_card_manager_add_card(m, card);
		} else {
			ms_warning("Android version is %i, failed to dlopen libOpenSLES, OpenSLES MS soundcard unavailable", sdk_version);
		}
	} else {
		ms_warning("Android version is %i, OpenSLES MS soundcard unavailable on Android < 4.4", sdk_version);
	}
}

static SLresult opensles_engine_init(OpenSLESContext *ctx) {
	SLresult result;

	result = slwCreateEngine(&(ctx->engineObject), 0, NULL, 0, NULL, NULL);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while creating SL engine", result);
		return result;
	}

	result = (*ctx->engineObject)->Realize(ctx->engineObject, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while realizing SL engine", result);
		return result;
	}

	result = (*ctx->engineObject)->GetInterface(ctx->engineObject, SLW_IID_ENGINE, &(ctx->engineEngine));

	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while getting SL engine interface", result);
		return result;
	}

	return result;
}

static OpenSLESContext* opensles_context_init() {
	OpenSLESContext* ctx = new OpenSLESContext();
	opensles_engine_init(ctx);
	return ctx;
} 

static void android_native_snd_card_init(MSSndCard *card) {

}

static void android_native_snd_card_uninit(MSSndCard *card) {
	OpenSLESContext *ctx = (OpenSLESContext*)card->data;
	ms_warning("Deletion of OpenSLES context [%p]", ctx);
	if (ctx->engineObject != NULL) {
                (*ctx->engineObject)->Destroy(ctx->engineObject);
                ctx->engineObject = NULL;
                ctx->engineEngine = NULL;
        }
}

static SLresult opensles_recorder_init(OpenSLESInputContext *ictx) {
	SLresult result;
	SLuint32 sample_rate = convertSamplerate(ictx->opensles_context->samplerate);

	SLDataLocator_IODevice loc_dev = {
		SL_DATALOCATOR_IODEVICE,
		SL_IODEVICE_AUDIOINPUT,
		SL_DEFAULTDEVICEID_AUDIOINPUT,
		NULL
	};

	SLDataSource audio_src = {
		&loc_dev,
		NULL
	};

	SLDataLocator_AndroidSimpleBufferQueue loc_bq = {
		SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
		2
	};

	SLDataFormat_PCM format_pcm = {
		SL_DATAFORMAT_PCM,
		ictx->opensles_context->nchannels,
		sample_rate,
		SL_PCMSAMPLEFORMAT_FIXED_16,
		SL_PCMSAMPLEFORMAT_FIXED_16,
		SL_SPEAKER_FRONT_CENTER,
		SL_BYTEORDER_LITTLEENDIAN
	};

	SLDataSink audio_sink = {
		&loc_bq,
		&format_pcm
	};

	const SLInterfaceID ids[] = {
		SLW_IID_ANDROIDSIMPLEBUFFERQUEUE,
		SLW_IID_ANDROIDCONFIGURATION
	};

	const SLboolean req[] = {
		SL_BOOLEAN_TRUE,
		SL_BOOLEAN_TRUE
	};

	result = (*ictx->opensles_context->engineEngine)->CreateAudioRecorder(ictx->opensles_context->engineEngine, &ictx->recorderObject, &audio_src, &audio_sink, 2, ids, req);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while creating the audio recorder", result);
		return result;
	}

	result = (*ictx->recorderObject)->GetInterface(ictx->recorderObject, SLW_IID_ANDROIDCONFIGURATION, &ictx->recorderConfig);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while getting the recorder's android config interface", result);
		return result;
	}

	result = (*ictx->recorderConfig)->SetConfiguration(ictx->recorderConfig, SL_ANDROID_KEY_RECORDING_PRESET, &ictx->streamType, sizeof(SLint32));
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while setting the audio recorder configuration", result);
		return result;
	}

	result = (*ictx->recorderObject)->Realize(ictx->recorderObject, SL_BOOLEAN_FALSE);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while realizing the audio recorder", result);
		return result;
	}

	result = (*ictx->recorderObject)->GetInterface(ictx->recorderObject, SLW_IID_RECORD, &ictx->recorderRecord);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while getting the audio recorder's interface", result);
		return result;
	}

	result = (*ictx->recorderObject)->GetInterface(ictx->recorderObject, SLW_IID_ANDROIDSIMPLEBUFFERQUEUE, &ictx->recorderBufferQueue);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while getting the audio recorder's buffer interface", result);
		return result;
	}

	return result;
}

static void compute_timespec(OpenSLESInputContext *ictx) {
	static int count = 0;
	uint64_t ns = ((1000 * ictx->read_samples) / (uint64_t) ictx->opensles_context->samplerate) * 1000000;
	MSTimeSpec ts;
	ts.tv_nsec = ns % 1000000000;
	ts.tv_sec = ns / 1000000000;
	double av_skew = ms_ticker_synchronizer_set_external_time(ictx->mTickerSynchronizer, &ts);
	if ((++count) % 100 == 0)
		ms_message("sound/wall clock skew is average=%f ms", av_skew);
}

static void opensles_recorder_callback(SLAndroidSimpleBufferQueueItf bq, void *context) {
	SLresult result;
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)context;

	if (ictx->mTickerSynchronizer == NULL) {
		MSFilter *obj = ictx->mFilter;
		ictx->mTickerSynchronizer = ms_ticker_synchronizer_new();
		ms_ticker_set_time_func(obj->ticker, (uint64_t (*)(void*))ms_ticker_synchronizer_get_corrected_time, ictx->mTickerSynchronizer);
	}
	ictx->read_samples += ictx->inBufSize / sizeof(int16_t);

	mblk_t *m = allocb(ictx->inBufSize, 0);
	memcpy(m->b_wptr, ictx->recBuffer[ictx->currentBuffer], ictx->inBufSize);
	m->b_wptr += ictx->inBufSize;

	ms_mutex_lock(&ictx->mutex);
	compute_timespec(ictx);
	putq(&ictx->q, m);
	ms_mutex_unlock(&ictx->mutex);

 	result = (*ictx->recorderBufferQueue)->Enqueue(ictx->recorderBufferQueue, ictx->recBuffer[ictx->currentBuffer], ictx->inBufSize);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while enqueueing record buffer", result);
	}
	ictx->currentBuffer = ictx->currentBuffer == 1 ? 0 : 1;
}

static SLresult opensles_recorder_callback_init(OpenSLESInputContext *ictx) {
	SLresult result;

	result = (*ictx->recorderBufferQueue)->RegisterCallback(ictx->recorderBufferQueue, opensles_recorder_callback, ictx);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while realizing the audio recorder", result);
		return result;
	}

	// in case already recording, stop recording and clear buffer queue
	result = (*ictx->recorderRecord)->SetRecordState(ictx->recorderRecord, SL_RECORDSTATE_STOPPED);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while stopping the audio recorder", result);
		return result;
	}

	result = (*ictx->recorderBufferQueue)->Clear(ictx->recorderBufferQueue);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while clearing the audio recorder buffer queue", result);
		return result;
	}

	result = (*ictx->recorderRecord)->SetRecordState(ictx->recorderRecord, SL_RECORDSTATE_RECORDING);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while starting the audio recorder", result);
		return result;
	}

	result = (*ictx->recorderBufferQueue)->Enqueue(ictx->recorderBufferQueue, ictx->recBuffer[0], ictx->inBufSize);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while enqueueing record buffer", result);
	}

	result = (*ictx->recorderBufferQueue)->Enqueue(ictx->recorderBufferQueue, ictx->recBuffer[1], ictx->inBufSize);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while enqueueing record buffer", result);
	}

	return result;
}

static OpenSLESInputContext* opensles_input_context_init() {
	OpenSLESInputContext* ictx = new OpenSLESInputContext();
	return ictx;
}

static void android_snd_read_init(MSFilter *obj) {
	OpenSLESInputContext *ictx = opensles_input_context_init();
	obj->data = ictx;
}

static void android_snd_read_preprocess(MSFilter *obj) {
	OpenSLESInputContext *ictx = (OpenSLESInputContext*) obj->data;
	ictx->mFilter = obj;
	ictx->read_samples = 0;
	opensles_recorder_init(ictx);
	opensles_recorder_callback_init(ictx);
	if (ictx->opensles_context->builtin_aec) { 
		//android_snd_read_activate_hardware_aec(obj);
	}
}

static void android_snd_read_process(MSFilter *obj) {
	OpenSLESInputContext *ictx = (OpenSLESInputContext*) obj->data;
	mblk_t *m;

	ms_mutex_lock(&ictx->mutex);
	while ((m = getq(&ictx->q)) != NULL) {
		ms_queue_put(obj->outputs[0], m);
	}
	ms_mutex_unlock(&ictx->mutex);
}

static void android_snd_read_postprocess(MSFilter *obj) {
	SLresult result;
	OpenSLESInputContext *ictx = (OpenSLESInputContext*)obj->data;

	ms_ticker_set_time_func(obj->ticker, NULL, NULL);
	ms_mutex_lock(&ictx->mutex);
	ms_ticker_synchronizer_destroy(ictx->mTickerSynchronizer);
	ictx->mTickerSynchronizer = NULL;
	ms_mutex_unlock(&ictx->mutex);

	if (ictx->aec) {
		JNIEnv *env = ms_get_jni_env();
		env->DeleteGlobalRef(ictx->aec);
		ictx->aec = NULL;
	}

	result = (*ictx->recorderRecord)->SetRecordState(ictx->recorderRecord, SL_RECORDSTATE_STOPPED);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while stopping the audio recorder", result);
	}

	result = (*ictx->recorderBufferQueue)->Clear(ictx->recorderBufferQueue);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("OpenSLES Error %u while clearing the audio recorder buffer queue", result);
	}

	if (ictx->recorderObject != NULL)
        {
                (*ictx->recorderObject)->Destroy(ictx->recorderObject);
                ictx->recorderObject = NULL;
                ictx->recorderRecord = NULL;
                ictx->recorderBufferQueue = NULL;
        }
}

static void android_snd_read_uninit(MSFilter *obj) {
	OpenSLESInputContext *ictx = (OpenSLESInputContext*)obj->data;
	delete ictx;
}

static int android_snd_read_set_sample_rate(MSFilter *obj, void *data) {
#ifndef NATIVE_USE_HARDWARE_RATE
	int *n = (int*)data;
	OpenSLESInputContext *ictx = (OpenSLESInputContext*)obj->data;
	ictx->opensles_context->samplerate = *n;
	return 0;
#else
	return -1;
#endif
}

static int android_snd_read_get_sample_rate(MSFilter *obj, void *data) {
	int *n = (int*)data;
	OpenSLESInputContext *ictx = (OpenSLESInputContext*)obj->data;
	*n = ictx->opensles_context->samplerate;
	return 0;
}

static int android_snd_read_set_nchannels(MSFilter *obj, void *data) {
	int *n = (int*)data;
	OpenSLESInputContext *ictx = (OpenSLESInputContext*)obj->data;
	ictx->opensles_context->nchannels = *n;
	return 0;
}

static int android_snd_read_get_nchannels(MSFilter *obj, void *data) {
	int *n = (int*)data;
	OpenSLESInputContext *ictx = (OpenSLESInputContext*)obj->data;
	*n = ictx->opensles_context->nchannels;
	return 0;
}

static int android_snd_read_hack_speaker_state(MSFilter *f, void *arg) {
	return 0;
}

static MSFilterMethod android_snd_read_methods[] = {
	{MS_FILTER_SET_SAMPLE_RATE, android_snd_read_set_sample_rate},
	{MS_FILTER_GET_SAMPLE_RATE, android_snd_read_get_sample_rate},
	{MS_FILTER_SET_NCHANNELS, android_snd_read_set_nchannels},
	{MS_FILTER_GET_NCHANNELS, android_snd_read_get_nchannels},
	{MS_AUDIO_CAPTURE_FORCE_SPEAKER_STATE, android_snd_read_hack_speaker_state},
	{0,NULL}
};

MSFilterDesc android_snd_opensles_read_desc = {
	MS_FILTER_PLUGIN_ID,
	"MSOpenSLESRecorder",
	"android sound source",
	MS_FILTER_OTHER,
	NULL,
	0,
	1,
	android_snd_read_init,
	android_snd_read_preprocess,
	android_snd_read_process,
	android_snd_read_postprocess,
	android_snd_read_uninit,
	android_snd_read_methods
};

static MSFilter* ms_android_snd_read_new() {
	MSFilter *f = ms_filter_new_from_desc(&android_snd_opensles_read_desc);
	return f;
}

static MSFilter *android_snd_card_create_reader(MSSndCard *card) {
	MSFilter *f = ms_android_snd_read_new();
	OpenSLESInputContext *ictx = static_cast<OpenSLESInputContext*>(f->data);
	ictx->setContext((OpenSLESContext*)card->data);
	return f;
}

static SLresult opensles_mixer_init(OpenSLESOutputContext *octx) {
	SLresult result;

        const SLuint32 nbInterface = 1;
        const SLInterfaceID ids[] = {SLW_IID_VOLUME};
        const SLboolean req[] = {SL_BOOLEAN_FALSE};
        result = (*octx->opensles_context->engineEngine)->CreateOutputMix(
                octx->opensles_context->engineEngine,
                &(octx->outputMixObject),
                nbInterface,
                ids,
                req);

        if (result != SL_RESULT_SUCCESS) {
			ms_error("OpenSLES Error %u while creating output mixer", result);
			return result;
		}

        result = (*octx->outputMixObject)->Realize(octx->outputMixObject, SL_BOOLEAN_FALSE);
        if (result != SL_RESULT_SUCCESS) {
        	ms_error("OpenSLES Error %u while realizing output mixer", result);
			return result;
        }

        return result;
}

static SLresult opensles_sink_init(OpenSLESOutputContext *octx) {
	SLresult result;
        SLuint32 sample_rate = convertSamplerate(octx->opensles_context->samplerate);

        SLDataFormat_PCM format_pcm = {
                SL_DATAFORMAT_PCM,
                octx->opensles_context->nchannels,
                sample_rate,
                SL_PCMSAMPLEFORMAT_FIXED_16,
                SL_PCMSAMPLEFORMAT_FIXED_16,
                SL_SPEAKER_FRONT_CENTER,
                SL_BYTEORDER_LITTLEENDIAN
        };

        SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
                SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 
                2
        };

        SLDataSource audio_src = {
                &loc_bufq,
                &format_pcm
        };

        SLDataLocator_OutputMix loc_outmix = {
                SL_DATALOCATOR_OUTPUTMIX,
                octx->outputMixObject
        };

        SLDataSink audio_sink = {
                &loc_outmix,
                NULL
        };

        const SLInterfaceID ids[] = {SLW_IID_VOLUME, SLW_IID_ANDROIDSIMPLEBUFFERQUEUE, SLW_IID_ANDROIDCONFIGURATION};
        const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
        result = (*octx->opensles_context->engineEngine)->CreateAudioPlayer(
                octx->opensles_context->engineEngine,
                &(octx->playerObject),
                &audio_src,
                &audio_sink,
                3,
                ids,
                req
                );
        if (result != SL_RESULT_SUCCESS) {
			ms_error("OpenSLES Error %u while creating ouput audio player", result);
			return result;
        }

	result = (*octx->playerObject)->GetInterface(octx->playerObject, SLW_IID_ANDROIDCONFIGURATION, &octx->playerConfig);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while realizing output sink", result);
		return result;
	}

	result = (*octx->playerConfig)->SetConfiguration(octx->playerConfig, SL_ANDROID_KEY_STREAM_TYPE, &octx->streamType, sizeof(SLint32));
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while realizing output sink", result);
		return result;
	}

	result = (*octx->playerObject)->Realize(octx->playerObject, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while realizing output sink", result);
		return result;
	}

	result = (*octx->playerObject)->GetInterface(octx->playerObject, SLW_IID_PLAY, &(octx->playerPlay));
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while getting output sink interface 1", result);
		return result;
	}

	result = (*octx->playerObject)->GetInterface(octx->playerObject, SLW_IID_ANDROIDSIMPLEBUFFERQUEUE, &(octx->playerBufferQueue));
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while getting output sink interface 2", result);
		return result;
	}

	return result;
}

static void opensles_player_callback(SLAndroidSimpleBufferQueueItf bq, void* context) {
	SLresult result;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext*)context;

	ms_mutex_lock(&octx->mutex);
	int ask = octx->outBufSize;
	int avail = ms_bufferizer_get_avail(&octx->buffer);
	int bytes = MIN(ask, avail);

	if ((octx->nbufs == 0) && (avail > (ask * 2))) {
		ms_warning("OpenSLES skipping %i bytes", avail - (ask * 2) );
		ms_bufferizer_skip_bytes(&octx->buffer, avail - (ask * 2));
	}

	if (avail != 0) {
		if ((octx->minBufferFilling == -1)) {
			octx->minBufferFilling = avail;
		} else if (avail < octx->minBufferFilling) {
			octx->minBufferFilling = avail;
		}
	}

	if (bytes > 0) {
		bytes = ms_bufferizer_read(&octx->buffer, octx->playBuffer[octx->currentBuffer], bytes);
	} else {
		/* we have an underrun (no more samples to deliver to the callback). We need to reset minBufferFilling*/
		octx->minBufferFilling = -1;
	}
	ms_mutex_unlock(&octx->mutex);
	octx->nbufs++;

	if (bytes == 0) bytes = ask;
 	result = (*octx->playerBufferQueue)->Enqueue(octx->playerBufferQueue, octx->playBuffer[octx->currentBuffer], bytes);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while adding buffer to output queue", result);
	}
	octx->currentBuffer = octx->currentBuffer == 1 ? 0 : 1;
}

static SLresult opensles_player_callback_init(OpenSLESOutputContext *octx) {
	SLresult result;

	result = (*octx->playerPlay)->SetPlayState(octx->playerPlay, SL_PLAYSTATE_STOPPED);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while stopping player", result);
		return result;
	}

	result = (*octx->playerBufferQueue)->Clear(octx->playerBufferQueue);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while clearing player buffer queue", result);
		return result;
	}

        result = (*octx->playerBufferQueue)->RegisterCallback(octx->playerBufferQueue, opensles_player_callback, octx);
        if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while registering player callback", result);
		return result;
	}

	result = (*octx->playerBufferQueue)->Enqueue(octx->playerBufferQueue, octx->playBuffer[0], octx->outBufSize);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while adding buffer to output queue", result);
	}

	result = (*octx->playerBufferQueue)->Enqueue(octx->playerBufferQueue, octx->playBuffer[1], octx->outBufSize);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while adding buffer to output queue", result);
	}

        result = (*octx->playerPlay)->SetPlayState(octx->playerPlay, SL_PLAYSTATE_PLAYING);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while starting player", result);
		return result;
	}

        return result;
}

static OpenSLESOutputContext* opensles_output_context_init() {
	OpenSLESOutputContext* octx = new OpenSLESOutputContext();
	return octx;
}

static MSFilter *android_snd_card_create_writer(MSSndCard *card) {
	MSFilter *f = ms_android_snd_write_new();
	OpenSLESOutputContext *octx = static_cast<OpenSLESOutputContext*>(f->data);
	octx->setContext((OpenSLESContext*)card->data);
	return f;
}

static void android_snd_write_init(MSFilter *obj){
	OpenSLESOutputContext *octx = opensles_output_context_init();
	obj->data = octx;
}

static void android_snd_write_uninit(MSFilter *obj){
	OpenSLESOutputContext *octx = (OpenSLESOutputContext*)obj->data;
	delete octx;
}

static int android_snd_write_set_sample_rate(MSFilter *obj, void *data) {
#ifndef NATIVE_USE_HARDWARE_RATE
	int *n = (int*)data;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext*)obj->data;
	octx->opensles_context->samplerate = *n;
	return 0;
#else
	return -1;
#endif
}

static int android_snd_write_get_sample_rate(MSFilter *obj, void *data) {
	int *n = (int*)data;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext*)obj->data;
	*n = octx->opensles_context->samplerate;
	return 0;
}

static int android_snd_write_set_nchannels(MSFilter *obj, void *data) {
	int *n = (int*)data;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext*)obj->data;
	octx->opensles_context->nchannels = *n;
	return 0;
}

static int android_snd_write_get_nchannels(MSFilter *obj, void *data) {
	int *n = (int*)data;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext*)obj->data;
	*n = octx->opensles_context->nchannels;
	return 0;
}

static void android_snd_write_preprocess(MSFilter *obj) {
	OpenSLESOutputContext *octx = (OpenSLESOutputContext*)obj->data;

	opensles_mixer_init(octx);
	opensles_sink_init(octx);
	opensles_player_callback_init(octx);

	octx->flowControlStart = obj->ticker->time;
	octx->minBufferFilling = -1;
	octx->nbufs = 0;
}

static int bytes_to_ms(OpenSLESOutputContext *octx, int bytes){
	return bytes * 1000 / (2 * octx->opensles_context->nchannels * octx->opensles_context->samplerate);
}

static void android_snd_write_process(MSFilter *obj) {
	OpenSLESOutputContext *octx = (OpenSLESOutputContext*)obj->data;

	ms_mutex_lock(&octx->mutex);
	ms_bufferizer_put_from_queue(&octx->buffer, obj->inputs[0]);

	if (((uint32_t)(obj->ticker->time - octx->flowControlStart)) >= flowControlIntervalMs) {
		int threshold = (flowControlThresholdMs * octx->opensles_context->nchannels * 2 * octx->opensles_context->samplerate) / 1000;
		ms_message("OpenSLES Time to flow control: minBufferFilling=%i, threshold=%i", octx->minBufferFilling, threshold);
		if (octx->minBufferFilling > threshold) {
			int drop = octx->minBufferFilling - (threshold/4); //keep a bit in order not to risk an underrun in the next period.
			ms_warning("OpenSLES Too many samples waiting in sound writer (minBufferFilling=%i ms, threshold=%i ms), dropping %i ms", 
					   bytes_to_ms(octx, octx->minBufferFilling), bytes_to_ms(octx, threshold), bytes_to_ms(octx, drop));
			ms_bufferizer_skip_bytes(&octx->buffer, drop);
		}
		octx->flowControlStart = obj->ticker->time;
		octx->minBufferFilling = -1;
	}
	ms_mutex_unlock(&octx->mutex);
}

static void android_snd_write_postprocess(MSFilter *obj) {
	SLresult result;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext*)obj->data;
	
	result = (*octx->playerPlay)->SetPlayState(octx->playerPlay, SL_PLAYSTATE_STOPPED);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while stopping player", result);
	}

	result = (*octx->playerBufferQueue)->Clear(octx->playerBufferQueue);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("OpenSLES Error %u while clearing player buffer queue", result);
	}

	if (octx->playerObject != NULL)
        {
                (*octx->playerObject)->Destroy(octx->playerObject);
                octx->playerObject = NULL;
                octx->playerPlay = NULL;
                octx->playerBufferQueue = NULL;
        }

	if (octx->outputMixObject != NULL)
        {
                (*octx->outputMixObject)->Destroy(octx->outputMixObject);
                octx->outputMixObject = NULL;
        }
}

static MSFilterMethod android_snd_write_methods[] = {
	{MS_FILTER_SET_SAMPLE_RATE, android_snd_write_set_sample_rate},
	{MS_FILTER_GET_SAMPLE_RATE, android_snd_write_get_sample_rate},
	{MS_FILTER_SET_NCHANNELS, android_snd_write_set_nchannels},
	{MS_FILTER_GET_NCHANNELS, android_snd_write_get_nchannels},
	{0,NULL}
};

MSFilterDesc android_snd_opensles_write_desc = {
	MS_FILTER_PLUGIN_ID,
	"MSOpenSLESPlayer",
	"android sound output",
	MS_FILTER_OTHER,
	NULL,
	1,
	0,
	android_snd_write_init,
	android_snd_write_preprocess,
	android_snd_write_process,
	android_snd_write_postprocess,
	android_snd_write_uninit,
	android_snd_write_methods
};

static MSFilter* ms_android_snd_write_new(void) {
	MSFilter *f = ms_filter_new_from_desc(&android_snd_opensles_write_desc);
	return f;
}

MSSndCardDesc android_native_snd_opensles_card_desc = {
	"libmedia",
	android_snd_card_detect,
	android_native_snd_card_init,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	android_snd_card_create_reader,
	android_snd_card_create_writer,
	android_native_snd_card_uninit
};

static MSSndCard* android_snd_card_new(void) {
	MSSndCard* obj;
	SoundDeviceDescription *d;
	
	obj = ms_snd_card_new(&android_native_snd_opensles_card_desc);
	obj->name = ms_strdup("android opensles sound card");
	d = sound_device_description_get();
	OpenSLESContext *context = opensles_context_init();
	if (d->flags & DEVICE_HAS_BUILTIN_OPENSLES_AEC) {
		obj->capabilities |= MS_SND_CARD_CAP_BUILTIN_ECHO_CANCELLER;
		context->builtin_aec = true;
	}
	obj->latency = 0; // Force software echo canceller if no builtin echo canceller
	obj->data = context;

	return obj;
}
