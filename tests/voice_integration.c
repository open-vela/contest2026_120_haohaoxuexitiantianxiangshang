#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "core/message_bus.h"
#include "voice/audio_capture.h"
#include "voice/audio_playback.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "voice/voice_channel.h"
#include "voice/voice_wake.h"
#include "voice_ui_bridge.h"
#include "agent_config.h"

static _Thread_local pid_t group = 10;
pid_t test_getpid(void) { return group; }
static atomic_int capture_live, playback_live, opens, asrs, synths;
static atomic_int asr_gate, asr_entered, tts_gate, tts_entered;
static atomic_int fail_open, fail_start, fail_read, fail_asr, fail_tts;
static atomic_int fail_alloc, fail_thread, tasks, registrations, dispatches;
static atomic_int workers, worker_result;
static atomic_int wake_phrase, dialogue_end;
static atomic_int fail_dispatch, sync_reply, pcm_pattern, prompt_bytes;
static atomic_int fail_playback, last_asr_bytes;
static atomic_int fail_playback_write, playback_stops, local_cues;
static atomic_int playback_gate, playback_entered;
static atomic_int playback_trace_enabled, playback_trace_count;
static char playback_trace[16];
#define LOCAL_PCM_SAMPLES (AGENT_TTS_WS_SAMPLE_RATE / 2)
static int16_t last_local_pcm[LOCAL_PCM_SAMPLES];
static size_t last_local_count;
static char submitted_chat[64], submitted_text[512];

static void wait_count(atomic_int *value, int wanted) {
    for (int i = 0; i < 10000 && atomic_load(value) < wanted; i++) usleep(1000);
    assert(atomic_load(value) >= wanted);
}
static void wait_zero(atomic_int *value) {
    for (int i = 0; i < 10000 && atomic_load(value) != 0; i++) usleep(1000);
    assert(atomic_load(value) == 0);
}
void *test_malloc(size_t n) {
    if (n == AGENT_VOICE_PCM_BUF_SIZE && atomic_exchange(&fail_alloc, 0)) return NULL;
    return malloc(n);
}
struct launch { void *(*fn)(void *); void *arg; pid_t group; };
static void *launch_thread(void *p) {
    struct launch v = *(struct launch *)p;
    free(p);
    group = v.group;
    return v.fn(v.arg);
}
int test_pthread_create(pthread_t *id, const pthread_attr_t *attr,
                        void *(*fn)(void *), void *arg) {
    if (atomic_exchange(&fail_thread, 0)) return EAGAIN;
    struct launch *v = malloc(sizeof(*v));
    assert(v);
    *v = (struct launch){fn, arg, group};
    int ret = pthread_create(id, attr, launch_thread, v);
    if (ret) free(v);
    return ret;
}
static void *task_thread(void *p) {
    struct launch v = *(struct launch *)p;
    free(p);
    v.fn(v.arg);
    atomic_fetch_sub(&tasks, 1);
    return NULL;
}
int agent_task_create(void *(*fn)(void *), const char *name,
                      int stack, void *arg, int prio) {
    (void)name; (void)stack; (void)prio;
    struct launch *v = malloc(sizeof(*v));
    assert(v);
    *v = (struct launch){fn, arg, group};
    atomic_fetch_add(&tasks, 1);
    pthread_t tid;
    int ret = test_pthread_create(&tid, NULL, task_thread, v);
    if (ret) { free(v); atomic_fetch_sub(&tasks, 1); return -1; }
    pthread_detach(tid);
    return 0;
}
int mimo_asr_register(void) { atomic_fetch_add(&registrations, 1); return 0; }
int mimo_tts_register(void) { atomic_fetch_add(&registrations, 1); return 0; }
const char *voice_asr_get_backend(void) { return "mimo"; }
voice_asr_stream_t *voice_asr_stream_open(void) { return NULL; }
int voice_asr_stream_send(voice_asr_stream_t *s, const unsigned char *p, size_t n)
{ (void)s; (void)p; (void)n; return 0; }
void voice_asr_stream_abort(voice_asr_stream_t *s) { (void)s; }
int voice_asr_stream_finish(voice_asr_stream_t *s, char *out, size_t n)
{ (void)s; snprintf(out, n, "stream"); return 0; }
int voice_asr_recognize(const unsigned char *p, size_t len, char *out, size_t n) {
    assert(p && len);
    atomic_store(&last_asr_bytes, (int)len);
    atomic_fetch_add(&asrs, 1);
    atomic_fetch_add(&asr_entered, 1);
    while (atomic_load(&asr_gate)) usleep(1000);
    usleep(10000);
    int ret = atomic_exchange(&fail_asr, 0) ? -EIO : 0;
    const char *text = "学习问题";
    int phrase = atomic_exchange(&wake_phrase, 0);
    if (phrase == 1) text = "你好，openvela";
    else if (phrase == 2) text = "你好，openvela，介绍 hello world";
    else if (atomic_exchange(&dialogue_end, 0)) text = "结束对话";
    snprintf(out, n, "%s", ret == 0 ? text : "");
    atomic_fetch_sub(&asr_entered, 1);
    return ret;
}
struct audio_capture { atomic_int started, aborted, reads; pid_t owner; };
audio_capture_t *audio_capture_open(const char *dev, unsigned rate,
                                    unsigned channels, unsigned bits) {
    (void)dev; assert(rate == 16000 && channels == 1 && bits == 16);
    if (atomic_exchange(&fail_open, 0)) return NULL;
    assert(atomic_load(&playback_live) == 0);
    assert(atomic_fetch_add(&capture_live, 1) == 0);
    atomic_fetch_add(&opens, 1);
    audio_capture_t *cap = calloc(1, sizeof(*cap)); assert(cap);
    cap->owner = group;
    return cap;
}
int audio_capture_start(audio_capture_t *cap) {
    assert(cap && cap->owner == group);
    if (atomic_exchange(&fail_start, 0)) return -EIO;
    atomic_store(&cap->started, 1);
    return 0;
}
int audio_capture_read(audio_capture_t *cap, void *buf, size_t n) {
    assert(cap && cap->owner == group);
    while (!atomic_load(&cap->started) && !atomic_load(&cap->aborted)) usleep(500);
    usleep(1000);
    if (atomic_load(&cap->aborted)) return -77;
    int frame = atomic_fetch_add(&cap->reads, 1);
    if (atomic_load(&fail_read) && frame > 2) {
        atomic_store(&fail_read, 0); return -EIO;
    }
    assert(n >= 3200);
    int16_t *samples = buf;
    for (int i = 0; i < 1600; i++) {
        int pattern = atomic_load(&pcm_pattern);
        /* pattern 3 = start-of-window transient only: four voiced frames,
         * enough to latch speech_seen but far short of a phrase. */
        samples[i] = ((pattern == 1 && frame >= 2 && frame < 9)
                      || (pattern == 2 && frame == 0)
                      || (pattern == 3 && frame >= 1 && frame < 5))
            ? (i % 2 ? 2200 : -2200) : 0;
    }
    return 3200;
}
int audio_capture_abort(audio_capture_t *cap) {
    assert(cap && cap->owner == group); atomic_store(&cap->aborted, 1); return 0;
}
void audio_capture_close(audio_capture_t *cap) {
    assert(cap && cap->owner == group);
    assert(atomic_fetch_sub(&capture_live, 1) == 1); free(cap);
}
struct audio_playback {
    pid_t owner;
    int stopped;
    size_t local_count;
    int16_t local_pcm[LOCAL_PCM_SAMPLES];
};
audio_playback_t *audio_playback_open(const char *dev, unsigned r, unsigned c, unsigned b) {
    (void)dev; assert(r == AGENT_TTS_WS_SAMPLE_RATE && c == 1 && b == 16);
    if (atomic_exchange(&fail_playback, 0)) return NULL;
    assert(atomic_load(&capture_live) == 0);
    assert(atomic_fetch_add(&playback_live, 1) == 0);
    audio_playback_t *pb = calloc(1, sizeof(*pb)); assert(pb); pb->owner = group; return pb;
}
int audio_playback_write(audio_playback_t *pb, const void *data, size_t n) {
    assert(pb->owner == group && data && !pb->stopped);
    if (n > 32) {
        assert(n <= 960 && n % 2 == 0);
        atomic_store(&playback_entered, 1);
        while (atomic_load(&playback_gate)) usleep(1000);
        int fail = atomic_exchange(&fail_playback_write, 0);
        if (fail) return fail == 1 ? -EIO : (int)n - 2;
        const int16_t *samples = data;
        for (size_t i = 0; i < n / 2; i++) assert(samples[i] >= -3276 && samples[i] <= 3276);
        assert(pb->local_count + n / 2 <= LOCAL_PCM_SAMPLES);
        memcpy(pb->local_pcm + pb->local_count, data, n);
        pb->local_count += n / 2;
        atomic_fetch_add(&prompt_bytes, (int)n);
    }
    return (int)n;
}
void audio_playback_stop(audio_playback_t *pb) {
    assert(pb->owner == group);
    pb->stopped = 1;
    atomic_fetch_add(&playback_stops, 1);
}
void audio_playback_close(audio_playback_t *pb) {
    assert(pb->owner == group);
    usleep(50000); /* includes device close / final nxplayer wait */
    assert(atomic_load(&capture_live) == 0);
    if (pb->local_count) {
        memcpy(last_local_pcm, pb->local_pcm, pb->local_count * sizeof(int16_t));
        last_local_count = pb->local_count;
        atomic_fetch_add(&local_cues, 1);
    }
    if (atomic_load(&playback_trace_enabled)) {
        int slot = atomic_fetch_add(&playback_trace_count, 1);
        assert(slot < (int)sizeof(playback_trace));
        playback_trace[slot] = pb->local_count ? 'N' : 'T';
    }
    atomic_store(&playback_entered, 0);
    assert(atomic_fetch_sub(&playback_live, 1) == 1); free(pb);
}
int voice_tts_speak_stream(const char *text, voice_tts_chunk_cb cb, void *p) {
    assert(text && *text); atomic_fetch_add(&synths, 1);
    atomic_store(&tts_entered, 1);
    while (atomic_load(&tts_gate)) usleep(1000);
    unsigned char pcm[32] = {0}; cb(pcm, sizeof(pcm), 1, p);
    atomic_store(&tts_entered, 0);
    return atomic_exchange(&fail_tts, 0) ? -EIO : 0;
}
int voice_tts_speak(const char *s, unsigned char *p, size_t cap, size_t *n)
{ (void)s; (void)p; (void)cap; atomic_fetch_add(&synths, 1); *n = 0; return -EIO; }
int message_bus_push_inbound(const agent_msg_t *msg) {
    if (atomic_exchange(&fail_dispatch, 0)) return -1;
    snprintf(submitted_chat, sizeof(submitted_chat), "%s", msg->chat_id);
    snprintf(submitted_text, sizeof(submitted_text), "%s", msg->content);
    if (atomic_exchange(&sync_reply, 0)) {
        assert(voice_wake_response_begin(msg->chat_id, "即时答案"));
        voice_wake_response_complete(msg->chat_id, 0);
        atomic_store(&dialogue_end, 1);
    }
    atomic_fetch_add(&dispatches, 1); free(msg->content); return 0;
}

static void start_wake(void) {
    group = 10; int before = atomic_load(&opens);
    assert(voice_wake_start() == 0); wait_count(&opens, before + 1); group = 21;
}
static void stop_wake(void) {
    assert(voice_wake_stop() == 0); wait_zero(&tasks);
    assert(!voice_wake_is_running() && atomic_load(&capture_live) == 0);
}
static void *do_start(void *arg) {
    (void)arg; group = 21;
    atomic_store(&worker_result, voice_channel_start());
    atomic_fetch_sub(&workers, 1); return NULL;
}
static void *do_stop(void *arg) {
    (void)arg; group = 21; char text[512];
    atomic_store(&worker_result, voice_channel_stop_with_text(text, sizeof(text), false));
    atomic_fetch_sub(&workers, 1); return NULL;
}
static void *do_speak(void *arg) {
    (void)arg; group = 21;
    atomic_store(&worker_result, voice_channel_speak("回答"));
    atomic_fetch_sub(&workers, 1); return NULL;
}
static pthread_t launch_worker(void *(*fn)(void *)) {
    pthread_t thread; atomic_store(&workers, 1);
    assert(test_pthread_create(&thread, NULL, fn, NULL) == 0); return thread;
}
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static void wait_ptt(enum voice_ui_ptt_state_e state) {
    struct voice_ui_ptt_status_s st;
    for (int i=0; i<10000; i++) {
        voice_ui_bridge_ptt_get_status(&st);
        if (st.state == state) return;
        usleep(1000);
    }
    fprintf(stderr, "PTT state=%d, expected=%d, result=%d\n", st.state, state, st.result);
    assert(false);
}

static void wait_speak_idle(void) {
    for (int i = 0; i < 10000; i++) {
        if (!voice_ui_bridge_is_speaking()) return;
        usleep(1000);
    }
    assert(false);
}

static int peak_between(size_t start, size_t end) {
    assert(end <= last_local_count);
    int peak = 0;
    for (size_t i = start; i < end; i++) {
        int amplitude = abs(last_local_pcm[i]);
        if (amplitude > peak) peak = amplitude;
    }
    return peak;
}

static uint32_t local_pcm_hash(void) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < last_local_count; i++) {
        uint16_t sample = (uint16_t)last_local_pcm[i];
        hash = (hash ^ (sample & 0xff)) * 16777619u;
        hash = (hash ^ (sample >> 8)) * 16777619u;
    }
    return hash;
}

static void check_notification_pcm(void) {
    size_t rate = AGENT_TTS_WS_SAMPLE_RATE;
    size_t first = rate * 140 / 1000;
    size_t gap = rate * 40 / 1000;
    size_t ramp = rate * 20 / 1000;
    size_t last = rate * 180 / 1000;
    size_t second = first + gap;
    assert(last_local_count == first + gap + last);
    assert(last_local_count <= rate / 2);
    assert(last_local_pcm[0] == 0 && last_local_pcm[first - 1] == 0);
    assert(last_local_pcm[second] == 0 && last_local_pcm[last_local_count - 1] == 0);
    assert(peak_between(first, second) == 0);
    size_t starts[] = {0, second};
    size_t lengths[] = {first, last};
    for (unsigned i = 0; i < 2; i++) {
        size_t start = starts[i], end = start + lengths[i];
        int body = peak_between(start + ramp, end - ramp);
        assert(body > 2000 && body <= 2457);
        assert(peak_between(start, start + ramp / 4) < body / 3);
        assert(peak_between(end - ramp / 4, end) < body / 3);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    assert(voice_channel_init() == 0);
    start_wake();
    int before = atomic_load(&asrs);
    int ret = voice_channel_speak("回答测试");
#ifdef BASELINE
    printf("baseline wake + TTS result=%d (recording busy=%d)\n", ret, -EBUSY);
    assert(ret == -EBUSY); stop_wake(); return 0;
#else
    assert(ret == 0 && atomic_load(&asrs) == before);
    stop_wake();
    puts("PASS TTS pauses wake capture without submitting discarded PCM");

    start_wake();
    for (int round=0; round<3; round++) {
        uint32_t id; double t=now_ms();
        assert(voice_ui_bridge_ptt_start(&id) == 0);
        assert(now_ms()-t < 50); wait_ptt(VOICE_UI_PTT_RECORDING);
        int open_count=atomic_load(&opens);
        assert(voice_channel_init() == 0 && atomic_load(&registrations) == 2);
        usleep(30000); assert(atomic_load(&opens) == open_count);
        t=now_ms(); assert(voice_ui_bridge_ptt_stop(id) == 0);
        assert(now_ms()-t < 50); wait_ptt(VOICE_UI_PTT_DONE);
        assert(voice_channel_speak("这一轮回答") == 0);
    }
    stop_wake();
    puts("PASS three real bridge PTT/ASR/TTS rounds with wake enabled; request calls <50ms");

    /* The endpoint gate only reaches the cloud when the window holds speech,
     * so give the fake microphone a voiced window here. */
    atomic_store(&pcm_pattern, 1);
    start_wake(); atomic_store(&asr_gate, 1); wait_count(&asr_entered, 1);
    atomic_store(&wake_phrase, 1); before=atomic_load(&synths);
    pthread_t worker=launch_worker(do_start); usleep(30000);
    assert(atomic_load(&workers) == 1 && atomic_load(&capture_live) == 0);
    atomic_store(&asr_gate, 0); pthread_join(worker, NULL);
    assert(atomic_load(&worker_result) == 0 && atomic_load(&synths) == before);
    assert(voice_channel_cancel() == 0); stop_wake();
    atomic_store(&pcm_pattern, 0);
    puts("PASS foreground waits for in-flight ASR; late wake match discarded");

    assert(voice_channel_start() == 0); usleep(10000);
    atomic_store(&asr_gate, 1); worker=launch_worker(do_stop); wait_count(&asr_entered, 1);
    assert(voice_channel_start() == -EBUSY);
    assert(voice_channel_speak("不能抢录音") == -EBUSY);
    atomic_store(&fail_asr, 1); atomic_store(&asr_gate, 0); pthread_join(worker, NULL);
    assert(atomic_load(&worker_result) == -EIO);
    assert(voice_channel_start() == 0 && voice_channel_cancel() == 0);
    puts("PASS ASR owns channel until completion; failure releases it for retry");

    atomic_store(&tts_gate, 1); worker=launch_worker(do_speak); wait_count(&tts_entered, 1);
    assert(voice_channel_start_background() == -EBUSY);
    pthread_t starter=launch_worker(do_start); usleep(30000);
    assert(atomic_load(&capture_live) == 0); atomic_store(&tts_gate, 0);
    pthread_join(worker, NULL); pthread_join(starter, NULL);
    assert(atomic_load(&capture_live) == 1 && voice_channel_cancel() == 0);
    puts("PASS capture waits through both TTS network and playback close");

    atomic_int *faults[]={&fail_open,&fail_start,&fail_alloc,&fail_thread};
    for (unsigned i=0; i<sizeof(faults)/sizeof(faults[0]); i++) {
        atomic_store(faults[i], 1); assert(voice_channel_start() < 0);
        assert(atomic_load(&capture_live) == 0);
        assert(voice_channel_start() == 0 && voice_channel_cancel() == 0);
    }
    atomic_store(&fail_read, 1); assert(voice_channel_start() == 0); usleep(30000);
    char text[512]; assert(voice_channel_stop_with_text(text,sizeof(text), false) == -EIO);
    assert(voice_channel_start() == 0 && voice_channel_cancel() == 0);
    puts("PASS capture open/start/allocation/thread/read failures all permit retry");

    assert(voice_channel_start() == 0); group=99;
    assert(voice_channel_stop_with_text(text,sizeof(text), false) == -EPERM);
    assert(voice_channel_cancel() == -EPERM && atomic_load(&capture_live) == 1);
    group=21; assert(voice_channel_cancel() == 0);
    atomic_store(&fail_tts,1); assert(voice_channel_speak("故障回答") == -EIO);
    assert(voice_channel_speak("重试回答") == 0);
    puts("PASS foreign task cannot close capture; TTS failure permits retry");

    char command[512];
    assert(voice_wake_extract_command("你好，openvela，介绍 hello world", command, sizeof(command)));
    assert(strcmp(command, "介绍 hello world") == 0);
    assert(voice_wake_matches("你好 openvela") && voice_wake_matches("你好，openvela"));
    assert(voice_wake_matches("Hello，OpenVela") && voice_wake_matches("小维同学"));
    assert(!voice_wake_matches("你好"));
    /* Contest compliance: the Xiaomi-branded wake words must no longer match. */
    assert(!voice_wake_matches("你好小米") && !voice_wake_matches("小米同学"));
    assert(voice_wake_extract_command("你好，openvela", command, sizeof(command)) && !command[0]);
    puts("PASS official wake phrase, case/separator folding, legacy alias and no Xiaomi-brand match");

    atomic_store(&pcm_pattern, 1);
    assert(voice_channel_start() == 0);
    for (int i = 0; i < 1000 && !voice_channel_utterance_done(); i++) usleep(1000);
    assert(voice_channel_utterance_done());
    assert(voice_channel_cancel() == 0);
    atomic_store(&pcm_pattern, 0);
    puts("PASS sustained speech followed by silence exposes an endpoint");

    atomic_store(&pcm_pattern, 2);
    assert(voice_channel_start() == 0); usleep(25000);
    assert(!voice_channel_utterance_done());
    assert(voice_channel_cancel() == 0);
    atomic_store(&pcm_pattern, 0);
    assert(voice_channel_start() == 0);
    for (int i = 0; i < 1000 && !voice_channel_utterance_done(); i++) usleep(1000);
    assert(voice_channel_utterance_done());
    assert(voice_channel_stop_with_text(command, sizeof(command), false) == 0);
    assert(atomic_load(&last_asr_bytes) == AGENT_VOICE_PCM_BUF_SIZE);
    puts("PASS startup spike is not an utterance; full ten-second PCM is retained before stopping");

    /* Endpoint gate: the wake listener re-arms on a timer, so a window with no
     * speech must not cost a TLS handshake plus an ASR request.  The local
     * heuristic in track_utterance() saw nothing here. */
    atomic_store(&pcm_pattern, 0);
    atomic_store(&last_asr_bytes, 0);
    assert(voice_channel_start() == 0);
    for (int i = 0; i < 1000 && !voice_channel_utterance_done(); i++) usleep(1000);
    assert(voice_channel_stop_with_text(command, sizeof(command), true) == -EAGAIN);
    assert(atomic_load(&last_asr_bytes) == 0);
    puts("PASS silent wake window is gated locally with no ASR request");

    /* Regression for the 2026-09-16 real-machine finding.  A window whose
     * only energy is the start-of-window transient still latches
     * speech_seen, so a gate keyed on speech_seen alone uploads 80 KB plus a
     * TLS handshake every few seconds of silence (measured: 16 uploads in
     * 69 s).  Four voiced frames latch the flag but leave only 300 ms of
     * cumulative voiced time in a 1300 ms window, which the gate must
     * reject. */
    atomic_store(&pcm_pattern, 3);
    atomic_store(&last_asr_bytes, 0);
    assert(voice_channel_start() == 0);
    for (int i = 0; i < 1000 && !voice_channel_utterance_done(); i++) usleep(1000);
    assert(voice_channel_utterance_done());
    assert(voice_channel_stop_with_text(command, sizeof(command), true) == -EAGAIN);
    assert(atomic_load(&last_asr_bytes) == 0);
    atomic_store(&pcm_pattern, 0);
    puts("PASS transient-only window is gated: speech_seen latches, voiced time does not");

    /* The same silent window still reaches the cloud for a user-triggered
     * dialogue, which passes require_speech=false. */
    atomic_store(&last_asr_bytes, 0);
    assert(voice_channel_start() == 0);
    for (int i = 0; i < 1000 && !voice_channel_utterance_done(); i++) usleep(1000);
    assert(voice_channel_stop_with_text(command, sizeof(command), false) == 0);
    assert(atomic_load(&last_asr_bytes) == AGENT_VOICE_PCM_BUF_SIZE);
    puts("PASS dialogue path is unaffected by the endpoint gate");

    /* Sustained speech must still pass the gate. */
    atomic_store(&pcm_pattern, 1);
    atomic_store(&last_asr_bytes, 0);
    assert(voice_channel_start() == 0); usleep(60000);
    assert(voice_channel_stop_with_text(command, sizeof(command), true) == 0);
    assert(atomic_load(&last_asr_bytes) > 0);
    atomic_store(&pcm_pattern, 0);
    puts("PASS sustained speech passes the endpoint gate and uploads once");

    before=atomic_load(&synths);
    assert(voice_channel_play_wake_prompt() == 0);
    assert(atomic_load(&synths) == before && atomic_load(&prompt_bytes) == 7200);
    assert(last_local_count == 3600 && local_pcm_hash() == 0x7b1f4979u);
    puts("PASS wake cue is bounded local PCM with no cloud TTS request");

    int asr_before = atomic_load(&asrs);
    int dispatch_before = atomic_load(&dispatches);
    int pcm_before = atomic_load(&prompt_bytes);
    assert(voice_channel_play_notification_prompt() == 0);
    assert(atomic_load(&synths) == before && atomic_load(&asrs) == asr_before);
    assert(atomic_load(&dispatches) == dispatch_before);
    assert(atomic_load(&prompt_bytes) - pcm_before == AGENT_TTS_WS_SAMPLE_RATE * 360 / 1000 * 2);
    check_notification_pcm();
    puts("PASS local reminder is 360 ms of bounded two-note PCM with silence and soft envelopes; no network");

    atomic_store(&fail_thread, 1);
    double chime_start = now_ms();
    assert(voice_ui_bridge_chime() == -EAGAIN && now_ms() - chime_start < 50);
    assert(!voice_ui_bridge_is_speaking());
    atomic_store(&playback_gate, 1);
    chime_start = now_ms();
    assert(voice_ui_bridge_chime() == 0 && now_ms() - chime_start < 50);
    wait_count(&playback_entered, 1);
    assert(voice_ui_bridge_is_speaking());
    assert(atomic_load(&playback_live) == 1 && atomic_load(&synths) == before);
    atomic_store(&playback_gate, 0); wait_speak_idle();
    check_notification_pcm();
    puts("PASS chime submission returns before blocked PCM playback; failed worker creation retries");

    atomic_store(&playback_trace_count, 0);
    atomic_store(&playback_trace_enabled, 1);
    atomic_store(&tts_gate, 1);
    before = atomic_load(&synths);
    int cues_before = atomic_load(&local_cues);
    assert(voice_ui_bridge_speak("queued first speech") == 0); wait_count(&tts_entered, 1);
    chime_start = now_ms();
    assert(voice_ui_bridge_chime() == 0 && now_ms() - chime_start < 50);
    assert(voice_ui_bridge_speak("queued second speech") == 0);
    chime_start = now_ms();
    assert(voice_ui_bridge_chime() == -EBUSY && now_ms() - chime_start < 50);
    atomic_store(&tts_gate, 0); wait_speak_idle();
    atomic_store(&playback_trace_enabled, 0);
    assert(atomic_load(&synths) == before + 2 && atomic_load(&local_cues) == cues_before + 1);
    assert(atomic_load(&playback_trace_count) == 3 && memcmp(playback_trace, "TNT", 3) == 0);
    puts("PASS typed queue serializes speech/chime/speech; full chime queue rejects without dropping speech");

    uint32_t chime_ptt;
    assert(voice_ui_bridge_ptt_start(&chime_ptt) == 0); wait_ptt(VOICE_UI_PTT_RECORDING);
    int recording_opens = atomic_load(&opens);
    chime_start = now_ms();
    assert(voice_ui_bridge_chime() == -EBUSY && now_ms() - chime_start < 50);
    assert(voice_channel_play_notification_prompt() == -EBUSY);
    assert(atomic_load(&capture_live) == 1 && atomic_load(&opens) == recording_opens);
    voice_ui_bridge_ptt_cancel(chime_ptt); wait_ptt(VOICE_UI_PTT_IDLE);
    assert(voice_channel_start() == 0);
    assert(voice_ui_bridge_chime() == 0); wait_speak_idle();
    assert(atomic_load(&capture_live) == 1);
    assert(voice_channel_cancel() == 0);
    assert(voice_ui_bridge_chime() == 0); wait_speak_idle();
    puts("PASS UI and direct foreground recordings retain ownership; rejected async chime permits retry");

    atomic_store(&tts_gate, 1); worker = launch_worker(do_speak); wait_count(&tts_entered, 1);
    chime_start = now_ms();
    assert(voice_channel_play_notification_prompt() == -EBUSY && now_ms() - chime_start < 50);
    atomic_store(&tts_gate, 0); pthread_join(worker, NULL);
    assert(voice_channel_play_notification_prompt() == 0);
    atomic_store(&fail_playback, 1);
    assert(voice_channel_play_notification_prompt() == -EIO);
    for (int fault = 1; fault <= 2; fault++) {
        int stops = atomic_load(&playback_stops);
        atomic_store(&fail_playback_write, fault);
        assert(voice_channel_play_notification_prompt() == -EIO);
        assert(atomic_load(&playback_stops) == stops + 1 && atomic_load(&playback_live) == 0);
        assert(voice_channel_play_notification_prompt() == 0);
    }
    atomic_store(&fail_playback, 1);
    assert(voice_ui_bridge_chime() == 0); wait_speak_idle();
    assert(voice_ui_bridge_chime() == 0); wait_speak_idle();
    assert(atomic_load(&capture_live) == 0 && atomic_load(&playback_live) == 0);
    puts("PASS busy playback, open/write/short-write failures release chime ownership and retry without TTS");

    /* The dialogue tests below drive the wake listener all the way to a
     * dispatch, and the endpoint gate only reaches the cloud when the window
     * holds speech, so keep the fake microphone voiced until they finish. */
    atomic_store(&pcm_pattern, 1);

    int sends = atomic_load(&dispatches);
    atomic_store(&wake_phrase, 2);
    start_wake(); wait_count(&dispatches, sends + 1);
    voice_dialogue_status_t ds;
    voice_wake_get_dialogue(&ds);
    assert(ds.active && ds.response_pending && ds.phase == VOICE_DIALOGUE_WAITING);
    assert(strcmp(submitted_text, "介绍 hello world") == 0);
    int open_count = atomic_load(&opens);
    voice_wake_response_complete("reminder", 0);
    voice_wake_response_complete("wake-stale", 0);
    voice_wake_notify_response_complete();
    usleep(100000);
    voice_wake_get_dialogue(&ds); assert(ds.response_pending);
    assert(atomic_load(&opens) == open_count && atomic_load(&dispatches) == sends + 1);
    for (int i = 0; i < 1700; i++) {
        voice_wake_get_dialogue(&ds);
        if (ds.phase == VOICE_DIALOGUE_SLOW) break;
        usleep(10000);
    }
    assert(ds.phase == VOICE_DIALOGUE_SLOW && ds.response_pending);
    assert(atomic_load(&opens) == open_count);
    assert(voice_wake_response_begin(submitted_chat, "稍慢但有效的答案"));
    voice_wake_get_dialogue(&ds);
    assert(strstr(ds.reply, "有效") && ds.phase == VOICE_DIALOGUE_SPEAKING);
    assert(voice_channel_speak(ds.reply) == 0);
    assert(atomic_load(&opens) == open_count);
    atomic_store(&dialogue_end, 1);
    voice_wake_response_complete(submitted_chat, 0);
    usleep(250000); stop_wake();
    puts("PASS slow reply keeps recording closed; reminder/stale completion cannot release it");

    sends = atomic_load(&dispatches); atomic_store(&wake_phrase, 2);
    start_wake(); wait_count(&dispatches, sends + 1);
    voice_wake_cancel_dialogue(); open_count = atomic_load(&opens);
    usleep(200000); assert(atomic_load(&opens) == open_count);
    assert(!voice_wake_response_begin(submitted_chat, "取消后的晚到答案"));
    voice_wake_response_complete(submitted_chat, 0);
    stop_wake();
    puts("PASS cancelling a pending dialogue suppresses late TTS and does not re-enqueue");

    sends = atomic_load(&dispatches); atomic_store(&wake_phrase, 2);
    atomic_store(&sync_reply, 1);
    start_wake(); wait_count(&dispatches, sends + 1);
    usleep(250000); voice_wake_get_dialogue(&ds); assert(!ds.response_pending);
    stop_wake();
    puts("PASS synchronous response during enqueue is not lost");

    sends = atomic_load(&dispatches); atomic_store(&wake_phrase, 2);
    atomic_store(&fail_dispatch, 1); start_wake();
    for (int i = 0; i < 6000; i++) {
        voice_wake_get_dialogue(&ds);
        if (ds.phase == VOICE_DIALOGUE_ERROR) break;
        usleep(1000);
    }
    assert(ds.phase == VOICE_DIALOGUE_ERROR && !ds.response_pending);
    assert(atomic_load(&dispatches) == sends); stop_wake();
    puts("PASS inbound enqueue failure clears the pending request and reports an error");

    sends = atomic_load(&dispatches); atomic_store(&wake_phrase, 1);
    atomic_store(&fail_playback, 1); atomic_store(&pcm_pattern, 1);
    start_wake(); wait_count(&dispatches, sends + 1);
    voice_wake_get_dialogue(&ds); assert(ds.response_pending);
    voice_wake_cancel_dialogue(); voice_wake_response_complete(submitted_chat, 0);
    stop_wake(); atomic_store(&pcm_pattern, 0);
    puts("PASS local cue failure does not discard the wake event or the user's question");

    voice_wake_set_frontend_busy(true);
    group = 10; assert(voice_wake_start() == 0); group = 21;
    open_count = atomic_load(&opens); usleep(150000);
    assert(atomic_load(&opens) == open_count);
    voice_wake_set_frontend_busy(false); wait_count(&opens, open_count + 1);
    stop_wake();
    puts("PASS UI waiting/TTS ownership suppresses background wake recording");

    start_wake(); stop_wake();
    puts("PASS listener restart after dialogue and playback failures");
    assert(atomic_load(&capture_live) == 0 && atomic_load(&playback_live) == 0);
    return 0;
#endif
}
