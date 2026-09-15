/****************************************************************************
 * voice_ui_bridge.c - Voice glue between the UI and ai_agent's voice channel
 *
 * voice_channel_start() requires voice_channel_init() to have run (it
 * registers the MiMo ASR/TTS backends and resets state). ai_agent normally
 * does that at boot, but the UI can be running without it, so the bridge
 * initializes the channel lazily on first use.
 *
 * Starting capture can wait for TLS/TTS, and stopping it joins the capture
 * thread and runs ASR. All recorder operations run on one serial worker;
 * the UI only submits requests and polls snapshots. Playback has its own worker.
 * No worker calls LVGL, including lv_async_call().
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "voice_ui_bridge.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SPEAK_TEXT_MAX 512
#define SPEAK_SLOTS    2

/* voice_channel_speak() runs the TTS HTTP request and PCM playback on this
 * thread, so it needs room comparable to ai_agent's AGENT_VOICE_STACK.
 */

#define SPEAK_STACK    (32 * 1024)
#define PTT_STACK      (32 * 1024)
#define SPEAK_PRIO     50

/****************************************************************************
 * External Function Prototypes
 ****************************************************************************/

/* ai_agent: packages/ai_agent/src/voice/voice_channel.c */

extern int voice_channel_init(void);
extern int voice_channel_start(void);
/* PTT is an explicit user action, so the endpoint gate stays off here:
 * the third argument mirrors the dialogue path in lvgl_ui_channel.c.
 */
extern int voice_channel_stop_with_text(char *text_out, size_t text_cap,
                                        bool require_speech);
extern int voice_channel_speak(const char *text);
extern int voice_channel_play_notification_prompt(void);

/* ai_agent: packages/ai_agent/src/voice/voice_wake.c */

extern int voice_wake_start(void);
extern int voice_wake_stop(void);
extern bool voice_wake_is_running(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_channel_ready;
static pthread_mutex_t g_channel_lock = PTHREAD_MUTEX_INITIALIZER;

/* The recorder worker owns the backend until start/stop/ASR has returned.
 * In particular, the backend may mark itself idle before ASR is finished;
 * the bridge must not permit another start during that interval.
 */

static pthread_mutex_t g_ptt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ptt_cond;
static bool g_ptt_cond_ready;
static bool g_ptt_worker_started;
static bool g_ptt_pending;
static bool g_ptt_stop_requested;
static bool g_ptt_cancelled;
static uint32_t g_legacy_request_id;
static struct voice_ui_ptt_status_s g_ptt;

/* Typed playback requests prevent local cues from reaching cloud TTS. */

enum speak_request_kind_e
{
  SPEAK_REQUEST_TTS = 0,
  SPEAK_REQUEST_CHIME
};

struct speak_request_s
{
  enum speak_request_kind_e kind;
  char text[SPEAK_TEXT_MAX];
};

static pthread_mutex_t g_speak_lock = PTHREAD_MUTEX_INITIALIZER;

/* Initialized in ensure_speak_worker() rather than with
 * PTHREAD_COND_INITIALIZER, which does not name every field of NuttX's
 * pthread_cond_t.
 */

static pthread_cond_t g_speak_cond;
static bool g_speak_cond_ready;
static struct speak_request_s g_speak_queue[SPEAK_SLOTS];
static int g_speak_head;
static int g_speak_count;
static bool g_speak_worker_started;
static bool g_speaking;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool ensure_channel(void)
{
  bool ready;

  pthread_mutex_lock(&g_channel_lock);
  if (!g_channel_ready && voice_channel_init() == 0)
    {
      g_channel_ready = true;
    }

  ready = g_channel_ready;
  pthread_mutex_unlock(&g_channel_lock);
  return ready;
}

static int create_voice_worker(void *(*worker)(void *), size_t stack_size)
{
  pthread_attr_t attr;
  pthread_t tid;
  int ret;
#ifdef __NuttX__
  struct sched_param param;
#endif

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      return ret;
    }

  ret = pthread_attr_setstacksize(&attr, stack_size);
  if (ret == 0)
    {
      ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    }

#ifdef __NuttX__
  /* Keep network/audio work below the UI's default priority (100). */

  param.sched_priority = SPEAK_PRIO;
  if (ret == 0)
    {
      ret = pthread_attr_setschedparam(&attr, &param);
    }

  if (ret == 0)
    {
      ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    }
#endif

  if (ret == 0)
    {
      ret = pthread_create(&tid, &attr, worker, NULL);
    }

  pthread_attr_destroy(&attr);
  return ret;
}

static bool ptt_busy(void)
{
  return g_ptt.state == VOICE_UI_PTT_STARTING ||
         g_ptt.state == VOICE_UI_PTT_RECORDING ||
         g_ptt.state == VOICE_UI_PTT_RECOGNIZING ||
         g_ptt.state == VOICE_UI_PTT_CANCELLING;
}

static void *ptt_worker(void *arg)
{
  char text[VOICE_UI_PTT_TEXT_MAX];
  int ret;

  (void)arg;
  pthread_mutex_lock(&g_ptt_lock);

  for (; ; )
    {
      while (!g_ptt_pending)
        {
          pthread_cond_wait(&g_ptt_cond, &g_ptt_lock);
        }

      g_ptt_pending = false;

      /* Closing the page before the worker starts needs no backend call. */

      if (g_ptt_cancelled)
        {
          g_ptt.state = VOICE_UI_PTT_IDLE;
          pthread_cond_broadcast(&g_ptt_cond);
          continue;
        }

      pthread_mutex_unlock(&g_ptt_lock);
      ret = ensure_channel() ? voice_channel_start() : -ENODEV;
      pthread_mutex_lock(&g_ptt_lock);

      if (ret < 0)
        {
          g_ptt.result = ret;
          g_ptt.state = g_ptt_cancelled ? VOICE_UI_PTT_IDLE :
                                         VOICE_UI_PTT_START_FAILED;
          pthread_cond_broadcast(&g_ptt_cond);
          continue;
        }

      if (!g_ptt_stop_requested)
        {
          g_ptt.state = VOICE_UI_PTT_RECORDING;
        }

      pthread_cond_broadcast(&g_ptt_cond);
      while (!g_ptt_stop_requested)
        {
          pthread_cond_wait(&g_ptt_cond, &g_ptt_lock);
        }

      /* A release or cancel during slow start is retained by the flags.
       * Never hold the UI-facing lock across capture shutdown or HTTPS.
       */

      pthread_mutex_unlock(&g_ptt_lock);
      memset(text, 0, sizeof(text));
      ret = voice_channel_stop_with_text(text, sizeof(text), false);
      pthread_mutex_lock(&g_ptt_lock);

      g_ptt.result = ret;
      if (g_ptt_cancelled)
        {
          g_ptt.state = VOICE_UI_PTT_IDLE;
          g_ptt.text[0] = '\0';
        }
      else
        {
          g_ptt.state = ret < 0 ? VOICE_UI_PTT_ASR_FAILED :
                                 VOICE_UI_PTT_DONE;
          if (ret >= 0)
            {
              memcpy(g_ptt.text, text, sizeof(g_ptt.text));
              g_ptt.text[sizeof(g_ptt.text) - 1] = '\0';
            }
        }

      pthread_cond_broadcast(&g_ptt_cond);
    }

  return NULL;
}

/* Called with g_ptt_lock held. A failed create leaves the next press free
 * to retry, and the condition variable is initialized only once.
 */

static int ensure_ptt_worker(void)
{
  int ret;

  if (g_ptt_worker_started)
    {
      return 0;
    }

  if (!g_ptt_cond_ready)
    {
      ret = pthread_cond_init(&g_ptt_cond, NULL);
      if (ret != 0)
        {
          return ret;
        }

      g_ptt_cond_ready = true;
    }

  ret = create_voice_worker(ptt_worker, PTT_STACK);
  if (ret == 0)
    {
      g_ptt_worker_started = true;
    }

  return ret;
}

static void *speak_worker(void *arg)
{
  (void)arg;

  for (; ; )
    {
      struct speak_request_s request;
      int ret;

      pthread_mutex_lock(&g_speak_lock);

      while (g_speak_count == 0)
        {
          pthread_cond_wait(&g_speak_cond, &g_speak_lock);
        }

      request = g_speak_queue[g_speak_head];
      g_speak_head = (g_speak_head + 1) % SPEAK_SLOTS;
      g_speak_count--;
      g_speaking = true;

      pthread_mutex_unlock(&g_speak_lock);

      /* Only this worker opens playback; local cues never synthesize text. */

      ret = ensure_channel() ? 0 : -ENODEV;
      if (ret == 0)
        {
          ret = request.kind == SPEAK_REQUEST_CHIME ?
                voice_channel_play_notification_prompt() :
                voice_channel_speak(request.text);
        }

      if (ret != 0)
        {
          syslog(LOG_WARNING, "[ui_voice] %s failed: %d\n",
                 request.kind == SPEAK_REQUEST_CHIME ? "local chime" : "TTS",
                 ret);
        }

      pthread_mutex_lock(&g_speak_lock);
      g_speaking = false;
      pthread_mutex_unlock(&g_speak_lock);
    }

  return NULL;
}

static int ensure_speak_worker(void)
{
  int ret;

  if (g_speak_worker_started)
    {
      return 0;
    }

  if (!g_speak_cond_ready)
    {
      ret = pthread_cond_init(&g_speak_cond, NULL);
      if (ret != 0)
        {
          syslog(LOG_ERR, "[ui_voice] cond init failed: %d\n", ret);
          return ret;
        }

      g_speak_cond_ready = true;
    }

  ret = create_voice_worker(speak_worker, SPEAK_STACK);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[ui_voice] cannot start playback worker: %d\n", ret);
      return ret;
    }

  g_speak_worker_started = true;
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int voice_ui_bridge_init(void)
{
  /* Initialize eagerly so the first PTT press is not delayed by backend
   * registration. Failure is not fatal: ensure_channel() retries later.
   */

  ensure_channel();
  return 0;
}

bool voice_ui_bridge_is_ready(void)
{
  bool ready;

  /* A UI timer must not wait for a worker's backend initialization. */

  if (pthread_mutex_trylock(&g_channel_lock) != 0)
    {
      return false;
    }

  ready = g_channel_ready;
  pthread_mutex_unlock(&g_channel_lock);
  return ready;
}

int voice_ui_bridge_ptt_start(uint32_t *request_id)
{
  int ret;

  if (request_id == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_ptt_lock);
  if (ptt_busy())
    {
      pthread_mutex_unlock(&g_ptt_lock);
      return -EBUSY;
    }

  ret = ensure_ptt_worker();
  if (ret != 0)
    {
      pthread_mutex_unlock(&g_ptt_lock);
      syslog(LOG_ERR, "[ui_voice] cannot start recorder worker: %d\n", ret);
      return -ret;
    }

  g_ptt.request_id++;
  if (g_ptt.request_id == 0)
    {
      g_ptt.request_id++;
    }

  *request_id = g_ptt.request_id;
  g_ptt.state = VOICE_UI_PTT_STARTING;
  g_ptt.result = 0;
  g_ptt.text[0] = '\0';
  g_ptt_pending = true;
  g_ptt_stop_requested = false;
  g_ptt_cancelled = false;
  pthread_cond_broadcast(&g_ptt_cond);
  pthread_mutex_unlock(&g_ptt_lock);
  return 0;
}

int voice_ui_bridge_ptt_stop(uint32_t request_id)
{
  int ret = 0;

  pthread_mutex_lock(&g_ptt_lock);
  if (request_id == 0 || request_id != g_ptt.request_id)
    {
      ret = -EINVAL;
    }
  else if (g_ptt.state == VOICE_UI_PTT_STARTING ||
           g_ptt.state == VOICE_UI_PTT_RECORDING)
    {
      g_ptt_stop_requested = true;
      g_ptt.state = VOICE_UI_PTT_RECOGNIZING;
      pthread_cond_broadcast(&g_ptt_cond);
    }
  else if (g_ptt.state == VOICE_UI_PTT_CANCELLING ||
           g_ptt.state == VOICE_UI_PTT_IDLE)
    {
      ret = -ECANCELED;
    }

  /* Repeated release and release after a start failure preserve the result. */

  pthread_mutex_unlock(&g_ptt_lock);
  return ret;
}

void voice_ui_bridge_ptt_cancel(uint32_t request_id)
{
  pthread_mutex_lock(&g_ptt_lock);
  if (request_id != 0 && request_id == g_ptt.request_id)
    {
      g_ptt.text[0] = '\0';
      if (ptt_busy())
        {
          g_ptt_cancelled = true;
          g_ptt_stop_requested = true;
          g_ptt.state = VOICE_UI_PTT_CANCELLING;
          pthread_cond_broadcast(&g_ptt_cond);
        }
      else
        {
          g_ptt.state = VOICE_UI_PTT_IDLE;
        }
    }

  pthread_mutex_unlock(&g_ptt_lock);
}

void voice_ui_bridge_ptt_get_status(struct voice_ui_ptt_status_s *status)
{
  if (status == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_ptt_lock);
  *status = g_ptt;
  pthread_mutex_unlock(&g_ptt_lock);
}

int voice_ui_bridge_start_recording(void)
{
  uint32_t request_id;
  int ret = voice_ui_bridge_ptt_start(&request_id);

  if (ret < 0)
    {
      return ret;
    }

  pthread_mutex_lock(&g_ptt_lock);
  while (g_ptt.request_id == request_id &&
         g_ptt.state == VOICE_UI_PTT_STARTING)
    {
      pthread_cond_wait(&g_ptt_cond, &g_ptt_lock);
    }

  if (g_ptt.request_id == request_id &&
      g_ptt.state == VOICE_UI_PTT_RECORDING)
    {
      g_legacy_request_id = request_id;
      ret = 0;
    }
  else
    {
      ret = g_ptt.request_id == request_id && g_ptt.result < 0 ?
            g_ptt.result : -ECANCELED;
    }

  pthread_mutex_unlock(&g_ptt_lock);
  return ret;
}

int voice_ui_bridge_stop_recording(char *text_out, size_t text_cap)
{
  uint32_t request_id;
  int ret;

  if (text_out == NULL || text_cap == 0)
    {
      return -EINVAL;
    }

  text_out[0] = '\0';

  pthread_mutex_lock(&g_ptt_lock);
  request_id = g_legacy_request_id;
  pthread_mutex_unlock(&g_ptt_lock);

  ret = voice_ui_bridge_ptt_stop(request_id);
  if (ret < 0)
    {
      return ret;
    }

  pthread_mutex_lock(&g_ptt_lock);
  while (g_ptt.request_id == request_id && ptt_busy())
    {
      pthread_cond_wait(&g_ptt_cond, &g_ptt_lock);
    }

  ret = -ECANCELED;
  if (g_ptt.request_id == request_id &&
      (g_ptt.state == VOICE_UI_PTT_DONE ||
       g_ptt.state == VOICE_UI_PTT_ASR_FAILED))
    {
      ret = g_ptt.result;
      if (ret >= 0)
        {
          snprintf(text_out, text_cap, "%s", g_ptt.text);
        }
    }

  if (g_legacy_request_id == request_id)
    {
      g_legacy_request_id = 0;
    }

  pthread_mutex_unlock(&g_ptt_lock);
  return ret;
}

/* Queue text for playback and return immediately. */

int voice_ui_bridge_speak(const char *text)
{
  int slot;
  int ret;

  if (text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_speak_lock);
  ret = ensure_speak_worker();
  if (ret != 0)
    {
      pthread_mutex_unlock(&g_speak_lock);
      return -ret;
    }

  if (g_speak_count >= SPEAK_SLOTS)
    {
      /* Drop the oldest: the newest reply is the one worth hearing. */

      if (g_speak_queue[g_speak_head].kind == SPEAK_REQUEST_CHIME)
        {
          syslog(LOG_WARNING,
                 "[ui_voice] queued chime skipped for newer speech\n");
        }

      g_speak_head = (g_speak_head + 1) % SPEAK_SLOTS;
      g_speak_count--;
    }

  slot = (g_speak_head + g_speak_count) % SPEAK_SLOTS;
  g_speak_queue[slot].kind = SPEAK_REQUEST_TTS;
  strncpy(g_speak_queue[slot].text, text, SPEAK_TEXT_MAX - 1);
  g_speak_queue[slot].text[SPEAK_TEXT_MAX - 1] = '\0';
  g_speak_count++;

  pthread_cond_signal(&g_speak_cond);
  pthread_mutex_unlock(&g_speak_lock);

  return 0;
}

int voice_ui_bridge_chime(void)
{
  int ret = pthread_mutex_trylock(&g_ptt_lock);
  if (ret != 0)
    {
      return -ret;
    }

  bool recording = ptt_busy();
  pthread_mutex_unlock(&g_ptt_lock);
  if (recording)
    {
      return -EBUSY;
    }

  ret = pthread_mutex_trylock(&g_speak_lock);
  if (ret != 0)
    {
      return -ret;
    }

  if (g_speak_count >= SPEAK_SLOTS)
    {
      pthread_mutex_unlock(&g_speak_lock);
      return -EBUSY;
    }

  ret = ensure_speak_worker();
  if (ret == 0)
    {
      int slot = (g_speak_head + g_speak_count) % SPEAK_SLOTS;
      g_speak_queue[slot].kind = SPEAK_REQUEST_CHIME;
      g_speak_queue[slot].text[0] = '\0';
      g_speak_count++;
      pthread_cond_signal(&g_speak_cond);
    }

  pthread_mutex_unlock(&g_speak_lock);
  return -ret;
}

bool voice_ui_bridge_is_speaking(void)
{
  bool busy;

  pthread_mutex_lock(&g_speak_lock);
  busy = g_speaking || g_speak_count > 0;
  pthread_mutex_unlock(&g_speak_lock);
  return busy;
}

int voice_ui_bridge_wake_start(void)
{
  if (!ensure_channel())
    {
      return -ENODEV;
    }

  return voice_wake_start();
}

int voice_ui_bridge_wake_stop(void)
{
  return voice_wake_stop();
}

bool voice_ui_bridge_wake_is_running(void)
{
  return voice_wake_is_running();
}

void voice_ui_bridge_get_dialogue(voice_dialogue_status_t *out)
{
  voice_wake_get_dialogue(out);
}

void voice_ui_bridge_cancel_dialogue(void)
{
  voice_wake_cancel_dialogue();
}

void voice_ui_bridge_set_frontend_busy(bool busy)
{
  voice_wake_set_frontend_busy(busy);
}
