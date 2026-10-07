/*
 * Minimal OpenSL ES implementation forwarding to SDL2 Audio (QueueAudio).
 * Designed for Unity / FMOD audio output on Android.
 */
#include "opensles.h"
#include "android_native.h"
#include "loader.h"
#include "shim.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SL_RESULT_SUCCESS 0
#define SL_RESULT_PARAMETER_INVALID 2
#define SL_RESULT_FEATURE_UNSUPPORTED 12

typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint16_t SLuint16;
typedef uint8_t  SLboolean;

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE 1

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED 2
#define SL_PLAYSTATE_PLAYING 3

typedef struct {
    uint32_t time_low;
    uint16_t time_mid;
    uint16_t time_hi_and_version;
    uint16_t clock_seq;
    uint8_t  node[6];
} SLInterfaceID_;
typedef const SLInterfaceID_ *SLInterfaceID;

static const SLInterfaceID_ s_iid_engine = { 0x8d2ea720, 0xc7e4, 0x11df, 0x88c0, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ s_iid_play = { 0xef0bd9a0, 0xc7e4, 0x11df, 0x933b, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ s_iid_bq = { 0x198e1a60, 0x1476, 0x11e0, 0xbd40, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ s_iid_config = { 0x895fe560, 0x7433, 0x11e0, 0xa9b3, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ s_iid_record = { 0xc0b33560, 0xc7e4, 0x11df, 0x9a85, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };

const SLInterfaceID SL_IID_ENGINE = &s_iid_engine;
const SLInterfaceID SL_IID_PLAY = &s_iid_play;
const SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &s_iid_bq;
const SLInterfaceID SL_IID_ANDROIDCONFIGURATION = &s_iid_config;
const SLInterfaceID SL_IID_RECORD = &s_iid_record;

static const void *deref_iid(const void *p) {
    if (!p) return NULL;
    if (p == &SL_IID_ENGINE) return &s_iid_engine;
    if (p == &SL_IID_PLAY) return &s_iid_play;
    if (p == &SL_IID_ANDROIDSIMPLEBUFFERQUEUE) return &s_iid_bq;
    if (p == &SL_IID_ANDROIDCONFIGURATION) return &s_iid_config;
    if (p == &SL_IID_RECORD) return &s_iid_record;
    return p;
}

static int match_iid(const void *a, const void *b) {
    if (!a || !b) return 0;
    a = deref_iid(a);
    b = deref_iid(b);
    if (a == b) return 1;
    return memcmp(a, b, sizeof(SLInterfaceID_)) == 0;
}

struct SLObjectItf_;
typedef const struct SLObjectItf_ * const * SLObjectItf;
struct SLEngineItf_;
typedef const struct SLEngineItf_ * const * SLEngineItf;
struct SLPlayItf_;
typedef const struct SLPlayItf_ * const * SLPlayItf;
struct SLAndroidSimpleBufferQueueItf_;
typedef const struct SLAndroidSimpleBufferQueueItf_ * const * SLAndroidSimpleBufferQueueItf;
struct SLAndroidConfigurationItf_;
typedef const struct SLAndroidConfigurationItf_ * const * SLAndroidConfigurationItf;

struct SLObjectItf_ {
    SLresult (*Realize)(SLObjectItf self, SLboolean async);
    SLresult (*Resume)(SLObjectItf self, SLboolean async);
    SLresult (*GetState)(SLObjectItf self, SLuint32 *pState);
    SLresult (*GetInterface)(SLObjectItf self, const void *iid, void *pInterface);
    SLresult (*RegisterCallback)(SLObjectItf self, void *callback, void *pContext);
    void (*AbortAsyncOperation)(SLObjectItf self);
    void (*Destroy)(SLObjectItf self);
    SLresult (*SetPriority)(SLObjectItf self, SLint32 priority, SLboolean preemptable);
    SLresult (*GetPriority)(SLObjectItf self, SLint32 *pPriority, SLboolean *pPreemptable);
    SLresult (*SetLossOfControlNotification)(SLObjectItf self, SLboolean notify);
};

struct SLEngineItf_ {
    SLresult (*CreateLEDDevice)(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateVibraDevice)(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateAudioPlayer)(SLEngineItf self, SLObjectItf *pPlayer, void *pAudioSrc, void *pAudioSnk, SLuint32 numInterfaces, const void **pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateAudioRecorder)(SLEngineItf self, SLObjectItf *pRecorder, void *pAudioSrc, void *pAudioSnk, SLuint32 numInterfaces, const void **pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateMidiPlayer)(SLEngineItf self, SLObjectItf *pPlayer, void *pMIDISrc, void *pBankSrc, void *pAudioOutput, void *pVibra, void *pLEDArray, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateListener)(SLEngineItf self, SLObjectItf *pListener, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*Create3DGroup)(SLEngineItf self, SLObjectItf *pGroup, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateOutputMix)(SLEngineItf self, SLObjectItf *pMix, SLuint32 numInterfaces, const void **pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateMetadataExtractor)(SLEngineItf self, SLObjectItf *pMetadataExtractor, void *pDataSource, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateExtensionObject)(SLEngineItf self, SLObjectItf *pObject, void *pParameters, SLuint32 objectID, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*QueryNumSupportedEngineInterfaces)(SLEngineItf self, SLuint32 *pNumSupportedInterfaces);
    SLresult (*QuerySupportedEngineInterface)(SLEngineItf self, SLuint32 index, void *pInterfaceId);
};

typedef void (*slAndroidSimpleBufferQueueCallback)(SLAndroidSimpleBufferQueueItf caller, void *pContext);
typedef struct {
    SLuint32 count;
    SLuint32 index;
} SLAndroidSimpleBufferQueueState;

/* Audio state */
static slAndroidSimpleBufferQueueCallback s_bq_cb = NULL;
static void *s_bq_ctx = NULL;
static volatile int s_play_state = SL_PLAYSTATE_STOPPED;
static pthread_t s_audio_th = 0;
static volatile int s_audio_running = 0;
static volatile int s_enq_count = 0;   /* total Enqueue calls */
static volatile int s_enq_bytes = 0;   /* total bytes queued */
static int s_enq_logged = 0;           /* how many we have logged so far */

/* Rolling capture of the most recent buffers: the first buffers are silence (FMOD starts before
 * the game plays anything), so we keep the LAST few and write them out when the player is torn
 * down. That is the window that actually contains music. */
#define PCM_RING_SLOTS 16
static uint8_t s_pcm_ring[PCM_RING_SLOTS][4096];
static uint32_t s_pcm_ring_len[PCM_RING_SLOTS];
static int s_pcm_ring_head = 0;

static void pcm_ring_dump(void) {
    const char *dump = getenv("UNITY_DUMP_PCM");
    if (!dump || dump[0] != '1') return;
    FILE *fp = fopen("/tmp/pcm_dump.raw", "wb");
    if (!fp) { so_log("[sles] could not open /tmp/pcm_dump.raw"); return; }
    int total = 0;
    for (int i = 0; i < PCM_RING_SLOTS; i++) {
        int idx = (s_pcm_ring_head + i) % PCM_RING_SLOTS;   /* oldest -> newest */
        if (s_pcm_ring_len[idx]) { fwrite(s_pcm_ring[idx], 1, s_pcm_ring_len[idx], fp); total += s_pcm_ring_len[idx]; }
    }
    fclose(fp);
    so_log("[sles] PCM ring dumped: %d bytes (last %d buffers) to /tmp/pcm_dump.raw", total, PCM_RING_SLOTS);
}

void opensles_flush_dump(void) { pcm_ring_dump(); }

extern const struct SLAndroidSimpleBufferQueueItf_ *s_player_bq_itf_ptr;

/* Diagnostic: when UNITY_TONE=1, ignore FMOD and push a clean 440 Hz sine through the exact same
 * SDL/ALSA path. A clean tone => the fault is FMOD's PCM; a buzzing tone => the fault is the
 * SDL/ALSA output path itself. */
static void *tone_thread(void *arg) {
    (void)arg;
    so_log("[tone] 440 Hz test tone thread started (UNITY_TONE=1)");
    const int rate = 48000;
    int16_t buf[2048];
    double phase = 0.0;
    const double step = 2.0 * 3.14159265358979 * 440.0 / rate;
    for (int b = 0; b < 200; b++) {   /* ~4.3 s */
        for (int i = 0; i < 1024; i++) {
            int16_t v = (int16_t)(12000.0 * __builtin_sin(phase));
            phase += step;
            if (phase > 6.283185307179586) phase -= 6.283185307179586;
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }
        sdl_audio_queue(buf, sizeof buf);
        usleep(20000);
    }
    so_log("[tone] test tone finished");
    return NULL;
}

static void *audio_pump_thread(void *arg) {
    (void)arg;
    so_log("[sles] Audio pump thread started");
    /* KNOWN-GOOD pump parameters (from the build that played music correctly, before the
     * regression). The regression was `while (queued < 32768 && refills < 8)` with a 4 ms tick:
     * that makes FMOD's mixer render up to 8 buffers (170 ms of audio) per tick, ~10x real-time.
     * The 176 MB streaming BGM bundle cannot be read off the microSD that fast, so FMOD starves
     * and emits static - this is the crackle. These defaults restore the gentle ~1x pump:
     * at most ONE buffer per tick, aiming for ~85 ms queued. Tunable on-device:
     *   UNITY_AUDIO_TARGET (bytes), UNITY_AUDIO_REFILLS (per tick), UNITY_AUDIO_INTERVAL (us) */
    int target = 16384;      /* ~85 ms of 48 kHz stereo s16 */
    int max_refills = 1;     /* one buffer per tick - no burst */
    int interval_us = 5000;
    const char *e;
    if ((e = getenv("UNITY_AUDIO_TARGET")))   { int v = atoi(e); if (v >= 4096) target = v; }
    if ((e = getenv("UNITY_AUDIO_REFILLS")))  { int v = atoi(e); if (v >= 1) max_refills = v; }
    if ((e = getenv("UNITY_AUDIO_INTERVAL"))) { int v = atoi(e); if (v >= 1000) interval_us = v; }
    so_log("[sles] pump params: target=%d max_refills=%d interval=%dus", target, max_refills, interval_us);
    int unpaused = 0;
    int cb_calls = 0;
    int warned = 0;
    while (s_audio_running) {
        if (s_play_state == SL_PLAYSTATE_PLAYING && s_bq_cb) {
            int refills = 0;
            while (s_audio_running && s_play_state == SL_PLAYSTATE_PLAYING &&
                   sdl_audio_queued_size() < (uint32_t)target && refills < max_refills) {
                s_bq_cb((SLAndroidSimpleBufferQueueItf)&s_player_bq_itf_ptr, s_bq_ctx);
                cb_calls++;
                refills++;
            }
            if (!unpaused && sdl_audio_queued_size() > 0) {
                sdl_audio_pause(0);
                unpaused = 1;
            }
            /* After ~1s of pumping, if the FMOD callback never produced any PCM we are silent.
             * Say so once, explicitly: this is the difference between "FMOD never calls us" and
             * "FMOD calls us but enqueues nothing". */
            if (!warned && cb_calls > 200 && s_enq_count == 0) {
                so_log("[sles] WARNING: pump called FMOD callback %d times but 0 buffers enqueued "
                       "(FMOD has nothing to play / no sound started)", cb_calls);
                warned = 1;
            }
        } else {
            /* Not playing: allow the next Play to re-prime and unpause. */
            if (unpaused) { sdl_audio_pause(1); unpaused = 0; }
        }
        usleep(interval_us);
    }
    so_log("[sles] Audio pump thread stopped (cb_calls=%d enq=%d)", cb_calls, s_enq_count);
    return NULL;
}

/* --- BufferQueue Interface --- */
static SLresult bq_Enqueue(SLAndroidSimpleBufferQueueItf self, const void *pBuffer, SLuint32 size) {
    (void)self;
    if (pBuffer && size > 0) {
        sdl_audio_queue(pBuffer, size);
        s_enq_count++;
        s_enq_bytes += (int)size;
        /* Keep the newest buffers for a post-mortem dump (see pcm_ring_dump). */
        if (size <= sizeof(s_pcm_ring[0])) {
            memcpy(s_pcm_ring[s_pcm_ring_head], pBuffer, size);
            s_pcm_ring_len[s_pcm_ring_head] = size;
            s_pcm_ring_head = (s_pcm_ring_head + 1) % PCM_RING_SLOTS;
        }
        /* Rewrite the dump every ~1s. The game exits via _exit() on the controller combo, which
         * skips every cleanup path, so a periodic dump is the only way the file is guaranteed to
         * hold the window that actually contained music. */
        if ((s_enq_count % 1024) == 0) pcm_ring_dump();
        /* Log the first few and then every 512th call: proves FMOD is actually pushing PCM,
         * without drowning the log. Absence of these lines == FMOD never calls the callback. */
        if (s_enq_logged < 4 || (s_enq_count % 512) == 0) {
            const int16_t *s = (const int16_t *)pBuffer;
            so_log("[sles] bq_Enqueue #%d size=%u total=%dKB queued=%u  s0..3=%d,%d,%d,%d",
                   s_enq_count, size, s_enq_bytes / 1024, sdl_audio_queued_size(),
                   s[0], s[1], s[2], s[3]);
            s_enq_logged++;
        }
    } else {
        so_log("[sles] bq_Enqueue IGNORED (buf=%p size=%u)", pBuffer, size);
    }
    return SL_RESULT_SUCCESS;
}
static SLresult bq_Clear(SLAndroidSimpleBufferQueueItf self) {
    (void)self;
    return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(SLAndroidSimpleBufferQueueItf self, SLAndroidSimpleBufferQueueState *pState) {
    (void)self;
    if (pState) {
        pState->count = (sdl_audio_queued_size() > 0) ? 1 : 0;
        pState->index = 0;
    }
    return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(SLAndroidSimpleBufferQueueItf self, slAndroidSimpleBufferQueueCallback callback, void *pContext) {
    so_log("[sles] BufferQueue RegisterCallback cb=%p ctx=%p", (void *)callback, pContext);
    s_bq_cb = callback;
    s_bq_ctx = pContext;
    return SL_RESULT_SUCCESS;
}
static const struct SLAndroidSimpleBufferQueueItf_ {
    SLresult (*Enqueue)(SLAndroidSimpleBufferQueueItf, const void *, SLuint32);
    SLresult (*Clear)(SLAndroidSimpleBufferQueueItf);
    SLresult (*GetState)(SLAndroidSimpleBufferQueueItf, SLAndroidSimpleBufferQueueState *);
    SLresult (*RegisterCallback)(SLAndroidSimpleBufferQueueItf, slAndroidSimpleBufferQueueCallback, void *);
} s_bq_vtable = {
    bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback
};
const struct SLAndroidSimpleBufferQueueItf_ *s_player_bq_itf_ptr = &s_bq_vtable;

/* --- Play Interface --- */
static SLresult play_SetPlayState(SLPlayItf self, SLuint32 state) {
    so_log("[sles] Play SetPlayState -> %u", state);
    s_play_state = state;
    if (state == SL_PLAYSTATE_PLAYING) {
        if (!s_audio_running) {
            s_audio_running = 1;
            pthread_create(&s_audio_th, NULL, audio_pump_thread, NULL);
        }
        /* UNITY_TONE=1: bypass FMOD and push a clean sine to isolate FMOD from SDL/ALSA. */
        static int tone_started = 0;
        const char *tone = getenv("UNITY_TONE");
        if (!tone_started && tone && tone[0] == '1') {
            tone_started = 1;
            pthread_t tt;
            pthread_create(&tt, NULL, tone_thread, NULL);
            pthread_detach(tt);
        }
    } else {
        sdl_audio_pause(1);
    }
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(SLPlayItf self, SLuint32 *pState) {
    if (pState) *pState = s_play_state;
    return SL_RESULT_SUCCESS;
}
static SLresult play_dummy(void) { return SL_RESULT_SUCCESS; }
static const struct SLPlayItf_ {
    SLresult (*SetPlayState)(SLPlayItf, SLuint32);
    SLresult (*GetPlayState)(SLPlayItf, SLuint32 *);
    SLresult (*GetDuration)(SLPlayItf, void *);
    SLresult (*GetPosition)(SLPlayItf, void *);
    SLresult (*RegisterCallback)(SLPlayItf, void *, void *);
    SLresult (*SetCallbackEventsMask)(SLPlayItf, SLuint32);
    SLresult (*GetCallbackEventsMask)(SLPlayItf, SLuint32 *);
    SLresult (*SetMarkerPosition)(SLPlayItf, void *);
    SLresult (*ClearMarkerPosition)(SLPlayItf);
    SLresult (*GetMarkerPosition)(SLPlayItf, void *);
    SLresult (*SetPositionUpdatePeriod)(SLPlayItf, void *);
    SLresult (*GetPositionUpdatePeriod)(SLPlayItf, void *);
} s_play_vtable = {
    play_SetPlayState, play_GetPlayState, (void *)play_dummy, (void *)play_dummy,
    (void *)play_dummy, (void *)play_dummy, (void *)play_dummy, (void *)play_dummy,
    (void *)play_dummy, (void *)play_dummy, (void *)play_dummy, (void *)play_dummy
};
static const struct SLPlayItf_ *s_player_play_itf_ptr = &s_play_vtable;

/* --- Config Interface --- */
/* SL_ANDROID_KEY_* values (from OpenSLES_Android.h). FMOD reads the performance mode
 * back after setting it; returning success without filling *val left the caller with
 * uninitialised stack, so we answer with a sane default (LATENCY). */
#define SL_ANDROID_KEY_PERFORMANCE_MODE 0x00000003
#define SL_ANDROID_KEY_STREAM_TYPE      0x00000002
#define SL_ANDROID_PERFORMANCE_LATENCY  1

static SLresult cfg_SetConfiguration(SLAndroidConfigurationItf self, const void *key, const void *val, SLuint32 sz) {
    return SL_RESULT_SUCCESS;
}
static SLresult cfg_GetConfiguration(SLAndroidConfigurationItf self, const void *key, SLuint32 *sz, void *val) {
    uint32_t k = key ? *(const uint32_t *)key : 0;
    if (val) {
        if (k == SL_ANDROID_KEY_PERFORMANCE_MODE) *(uint32_t *)val = SL_ANDROID_PERFORMANCE_LATENCY;
        else if (k == SL_ANDROID_KEY_STREAM_TYPE) *(uint32_t *)val = 3; /* STREAM_MUSIC */
        else *(uint32_t *)val = 0;
    }
    if (sz) *sz = sizeof(uint32_t);
    return SL_RESULT_SUCCESS;
}
static const struct SLAndroidConfigurationItf_ {
    SLresult (*SetConfiguration)(SLAndroidConfigurationItf, const void *, const void *, SLuint32);
    SLresult (*GetConfiguration)(SLAndroidConfigurationItf, const void *, SLuint32 *, void *);
} s_cfg_vtable = {
    cfg_SetConfiguration, cfg_GetConfiguration
};
static const struct SLAndroidConfigurationItf_ *s_player_cfg_itf_ptr = &s_cfg_vtable;

/* --- Player Object --- */
static SLresult player_Realize(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }
static SLresult player_GetInterface(SLObjectItf self, const void *iid, void *pItf) {
    if (match_iid(iid, SL_IID_PLAY)) {
        so_log("[sles] Player GetInterface -> SL_IID_PLAY");
        *(const void ***)pItf = (const void **)&s_player_play_itf_ptr;
        return SL_RESULT_SUCCESS;
    }
    if (match_iid(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE)) {
        so_log("[sles] Player GetInterface -> SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
        *(const void ***)pItf = (const void **)&s_player_bq_itf_ptr;
        return SL_RESULT_SUCCESS;
    }
    if (match_iid(iid, SL_IID_ANDROIDCONFIGURATION)) {
        so_log("[sles] Player GetInterface -> SL_IID_ANDROIDCONFIGURATION");
        *(const void ***)pItf = (const void **)&s_player_cfg_itf_ptr;
        return SL_RESULT_SUCCESS;
    }
    so_log("[sles] Player GetInterface -> UNSUPPORTED %p", iid);
    return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void player_Destroy(SLObjectItf self) {
    so_log("[sles] Player Destroy");
    s_play_state = SL_PLAYSTATE_STOPPED;
    s_audio_running = 0;
    if (s_audio_th) {
        pthread_join(s_audio_th, NULL);
        s_audio_th = 0;
    }
    pcm_ring_dump();
    sdl_audio_pause(1);
    sdl_audio_close();
}
static const struct SLObjectItf_ s_player_obj_vtable = {
    player_Realize, (void *)play_dummy, (void *)play_dummy, player_GetInterface,
    (void *)play_dummy, (void *)play_dummy, player_Destroy, (void *)play_dummy,
    (void *)play_dummy, (void *)play_dummy
};
static const struct SLObjectItf_ *s_player_obj_ptr = &s_player_obj_vtable;

/* --- OutputMix Object --- */
static SLresult outmix_Realize(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }
static SLresult outmix_GetInterface(SLObjectItf self, const void *iid, void *pItf) { return SL_RESULT_FEATURE_UNSUPPORTED; }
static void outmix_Destroy(SLObjectItf self) {}
static const struct SLObjectItf_ s_outmix_obj_vtable = {
    outmix_Realize, (void *)play_dummy, (void *)play_dummy, outmix_GetInterface,
    (void *)play_dummy, (void *)play_dummy, outmix_Destroy, (void *)play_dummy,
    (void *)play_dummy, (void *)play_dummy
};
static const struct SLObjectItf_ *s_outmix_obj_ptr = &s_outmix_obj_vtable;

/* --- Engine Interface --- */
typedef struct {
    uint32_t formatType;     /* 1 = SL_DATAFORMAT_PCM */
    uint32_t numChannels;    /* 2 */
    uint32_t samplesPerSec;  /* milliHz: 44100000 or 48000000 */
    uint32_t bitsPerSample;  /* 16 */
    uint32_t containerSize;  /* 16 */
    uint32_t channelMask;
    uint32_t endianness;
} SLDataFormat_PCM;

static SLresult engine_CreateAudioPlayer(SLEngineItf self, SLObjectItf *pPlayer, void *pAudioSrc, void *pAudioSnk,
                                       SLuint32 numInterfaces, const void **pInterfaceIds, const SLboolean *pInterfaceRequired) {
    so_log("[sles] Engine CreateAudioPlayer");
    int rate = 48000, channels = 2;
    if (pAudioSrc) {
        void **src = (void **)pAudioSrc;
        if (src[1]) {
            SLDataFormat_PCM *pcm = (SLDataFormat_PCM *)src[1];
            /* Log the exact format FMOD asked for: a mismatch here (rate / bits / channels /
             * endianness) is the classic cause of "plays but sounds like static". */
            so_log("[sles] FMOD PCM format: type=%u ch=%u rate=%umHz bits=%u container=%u mask=0x%x endian=%u",
                   pcm->formatType, pcm->numChannels, pcm->samplesPerSec, pcm->bitsPerSample,
                   pcm->containerSize, pcm->channelMask, pcm->endianness);
            if (pcm->samplesPerSec) rate = pcm->samplesPerSec / 1000;
            if (pcm->numChannels) channels = pcm->numChannels;
        } else {
            so_log("[sles] FMOD pAudioSrc[1] (format) is NULL - defaulting %d Hz / %d ch", rate, channels);
        }
    } else {
        so_log("[sles] FMOD pAudioSrc is NULL - defaulting %d Hz / %d ch", rate, channels);
    }
    so_log("[sles] Initializing SDL audio: freq=%d channels=%d", rate, channels);
    sdl_audio_init(rate, channels, NULL, NULL);
    *pPlayer = (SLObjectItf)&s_player_obj_ptr;
    return SL_RESULT_SUCCESS;
}
static SLresult engine_CreateOutputMix(SLEngineItf self, SLObjectItf *pMix, SLuint32 numInterfaces,
                                     const void **pInterfaceIds, const SLboolean *pInterfaceRequired) {
    so_log("[sles] Engine CreateOutputMix");
    *pMix = (SLObjectItf)&s_outmix_obj_ptr;
    return SL_RESULT_SUCCESS;
}
static const struct SLEngineItf_ s_engine_itf_vtable = {
    (void *)play_dummy,        /* 0: CreateLEDDevice */
    (void *)play_dummy,        /* 1: CreateVibraDevice */
    engine_CreateAudioPlayer,  /* 2: CreateAudioPlayer */
    (void *)play_dummy,        /* 3: CreateAudioRecorder */
    (void *)play_dummy,        /* 4: CreateMidiPlayer */
    (void *)play_dummy,        /* 5: CreateListener */
    (void *)play_dummy,        /* 6: Create3DGroup */
    engine_CreateOutputMix,    /* 7: CreateOutputMix */
    (void *)play_dummy,        /* 8: CreateMetadataExtractor */
    (void *)play_dummy,        /* 9: CreateExtensionObject */
    (void *)play_dummy,        /* 10: QueryNumSupportedEngineInterfaces */
    (void *)play_dummy         /* 11: QuerySupportedEngineInterface */
};
static const struct SLEngineItf_ *s_engine_itf_ptr = &s_engine_itf_vtable;

/* --- Engine Object --- */
static SLresult engine_Realize(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }
static SLresult engine_GetInterface(SLObjectItf self, const void *iid, void *pItf) {
    if (match_iid(iid, SL_IID_ENGINE)) {
        *(const void ***)pItf = (const void **)&s_engine_itf_ptr;
        return SL_RESULT_SUCCESS;
    }
    return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void engine_Destroy(SLObjectItf self) {}
static const struct SLObjectItf_ s_engine_obj_vtable = {
    engine_Realize, (void *)play_dummy, (void *)play_dummy, engine_GetInterface,
    (void *)play_dummy, (void *)play_dummy, engine_Destroy, (void *)play_dummy,
    (void *)play_dummy, (void *)play_dummy
};
static const struct SLObjectItf_ *s_engine_obj_ptr = &s_engine_obj_vtable;

SLresult slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions, const void *pEngineOptions,
                        SLuint32 numInterfaces, const void *pInterfaceIds, const SLboolean *pInterfaceRequired) {
    so_log("[sles] slCreateEngine called -> returning engine object");
    if (pEngine) {
        *pEngine = (SLObjectItf)&s_engine_obj_ptr;

        /* FMOD on Android expects the OpenSL ES output plugin state to contain the sample rate and buffer size.
         * pEngine is passed as &plugin_state->engineObj (at offset 0x3c0 into FMOD's output plugin state).
         * The sample rate field is at offset 0x3f8 (pEngine + 0x38).
         * The buffer size field is at offset 0x3f0 (pEngine + 0x30).
         * If these are 0, FMOD aborts with FMOD_ERR_OUTPUT_INIT (0x3c) and falls back to NoSound!
         */
        uint32_t *p_sample_rate = (uint32_t *)((char *)pEngine + 0x38);
        uint32_t *p_buffer_size = (uint32_t *)((char *)pEngine + 0x30);
        if (*p_sample_rate == 0) *p_sample_rate = 48000;
        if (*p_buffer_size == 0) *p_buffer_size = 512;
        so_log("[sles] Primed FMOD plugin state: rate=%u bufsize=%u", *p_sample_rate, *p_buffer_size);
    }
    return SL_RESULT_SUCCESS;
}

void opensles_init(void) {
    shim_register("slCreateEngine", (void *)slCreateEngine);
    shim_register("SL_IID_ENGINE", (void *)&SL_IID_ENGINE);
    shim_register("SL_IID_PLAY", (void *)&SL_IID_PLAY);
    shim_register("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (void *)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE);
    shim_register("SL_IID_ANDROIDCONFIGURATION", (void *)&SL_IID_ANDROIDCONFIGURATION);
    shim_register("SL_IID_RECORD", (void *)&SL_IID_RECORD);
}
