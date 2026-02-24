/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Scheduler - Background thread for timers, alarms, reminders, and tasks
 *
 * Uses pthread_cond_timedwait with CLOCK_MONOTONIC for efficient scheduling.
 * The thread sleeps until the next event is due, consuming zero CPU when idle.
 */

#include "core/scheduler.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio/audio_backend.h"
#include "audio/chime.h"
#include "config/dawn_config.h"
#include "core/scheduler_db.h"
#include "core/session_manager.h"
#include "logging.h"
#include "tools/tool_registry.h"

#ifdef ENABLE_MULTI_CLIENT
#include "webui/webui_satellite.h"
#endif

#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/* Forward declaration for TTS */
extern void text_to_speech(char *text);
extern int tts_wait_for_completion(int timeout_ms);

/* Forward declaration for WebUI notification (implemented in Phase 4) */
#ifdef ENABLE_WEBUI
void scheduler_broadcast_notification(const sched_event_t *event, const char *text)
    __attribute__((weak));
void scheduler_broadcast_notification(const sched_event_t *event, const char *text) {
   (void)event;
   (void)text;
}
#endif

/* =============================================================================
 * Internal State
 * ============================================================================= */

static pthread_t scheduler_thread_id;
static pthread_mutex_t scheduler_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scheduler_cond;
static atomic_bool scheduler_running = false;
static atomic_bool scheduler_shutdown_flag = false;

/* Ringing state */
static atomic_bool alarm_ringing = false;
static sched_event_t ringing_event;
static pthread_mutex_t ringing_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Alarm sound state */
static atomic_bool alarm_sound_playing = false;
static atomic_bool alarm_sound_stop = false;

/* Alarm chime PCM buffers (generated at init via common lib) */
static dawn_chime_buf_t chime_buf;
static dawn_chime_buf_t alarm_tone_buf;

#define ALARM_GAP_MS 200

/* Forward declarations */
static void schedule_next_occurrence(const sched_event_t *fired_event);

/* =============================================================================
 * Announcement Generation
 * ============================================================================= */

static void generate_announcement_text(const sched_event_t *event, char *buf, size_t buf_size) {
   switch (event->event_type) {
      case SCHED_EVENT_TIMER:
         if (event->name[0]) {
            /* Avoid "Your pasta timer timer is done!" when name already contains "timer" */
            if (strstr(event->name, "timer") || strstr(event->name, "Timer"))
               snprintf(buf, buf_size, "Your %s is done!", event->name);
            else
               snprintf(buf, buf_size, "Your %s timer is done!", event->name);
         } else
            snprintf(buf, buf_size, "Timer complete!");
         break;
      case SCHED_EVENT_ALARM: {
         struct tm tm_info;
         localtime_r(&event->fire_at, &tm_info);
         char time_str[16];
         strftime(time_str, sizeof(time_str), "%I:%M %p", &tm_info);
         snprintf(buf, buf_size, "It's %s. Your alarm is going off.", time_str);
         break;
      }
      case SCHED_EVENT_REMINDER:
         if (event->message[0])
            snprintf(buf, buf_size, "Reminder: %s", event->message);
         else
            snprintf(buf, buf_size, "You have a reminder.");
         break;
      case SCHED_EVENT_TASK:
         if (event->tool_name[0])
            snprintf(buf, buf_size, "Scheduled task complete: %s %s", event->tool_action,
                     event->tool_value);
         else
            snprintf(buf, buf_size, "Scheduled task complete.");
         break;
   }
}

/* =============================================================================
 * Announcement Routing
 * ============================================================================= */

static void announce_event(const sched_event_t *event) {
   char announcement[512];
   generate_announcement_text(event, announcement, sizeof(announcement));

   LOG_INFO("scheduler: announcing event %lld (%s): %s", (long long)event->id,
            sched_event_type_to_str(event->event_type), announcement);

#ifdef ENABLE_MULTI_CLIENT
   /* Route to source satellite if connected */
   if (event->source_uuid[0]) {
      session_t *session = session_find_by_uuid(event->source_uuid);
      if (session) {
         if (!session->disconnected && session->tier == DAP2_TIER_1) {
            /* Tier 1 satellite: send text for local TTS */
            satellite_send_response(session, announcement);
            LOG_INFO("scheduler: announced to satellite %s", event->source_uuid);
         }
         /* Tier 2 not supported for alarms; skip */
         session_release(session);
      } else {
         LOG_INFO("scheduler: satellite %s disconnected, WebUI notification only",
                  event->source_uuid);
      }
   } else {
      /* Local mic source: play TTS on daemon speaker */
      char *tts_text = strdup(announcement);
      if (tts_text)
         text_to_speech(tts_text);
   }

   /* Announce everywhere if requested */
   if (event->announce_all) {
      /* Iterate sessions and send to connected Tier 1 satellites */
      for (int i = 0; i < MAX_SESSIONS; i++) {
         session_t *s = session_get((uint32_t)i);
         if (!s)
            continue;
         if (s->type == SESSION_TYPE_DAP2 && s->tier == DAP2_TIER_1 && !s->disconnected) {
            /* Skip the source satellite (already announced) */
            if (event->source_uuid[0] && strcmp(s->identity.uuid, event->source_uuid) == 0) {
               session_release(s);
               continue;
            }
            satellite_send_response(s, announcement);
         }
         session_release(s);
      }
   }
#else
   /* Local-only mode: play TTS on daemon speaker */
   char *tts_text = strdup(announcement);
   if (tts_text)
      text_to_speech(tts_text);
#endif

#ifdef ENABLE_WEBUI
   /* Always broadcast to WebUI clients */
   scheduler_broadcast_notification(event, announcement);
#endif
}

/* =============================================================================
 * Scheduled Task Execution
 * ============================================================================= */

/**
 * @brief Execute a scheduled task by calling its tool callback
 *
 * Validates TOOL_CAP_SCHEDULABLE and tool_registry_is_enabled() at execution time.
 * Never executes TOOL_CAP_DANGEROUS tools.
 *
 * @param event The task event containing tool_name, tool_action, tool_value
 * @return 0 on success, -1 on validation failure or execution error
 */
static int scheduler_execute_task(sched_event_t *event) {
   if (!event->tool_name[0]) {
      LOG_WARNING("scheduler: task %lld has no tool_name", (long long)event->id);
      return -1;
   }

   /* Look up tool metadata */
   const tool_metadata_t *meta = tool_registry_find(event->tool_name);
   if (!meta) {
      LOG_WARNING("scheduler: tool '%s' not found (task %lld)", event->tool_name,
                  (long long)event->id);
      return -1;
   }

   /* Validate SCHEDULABLE capability */
   if (!(meta->capabilities & TOOL_CAP_SCHEDULABLE)) {
      LOG_WARNING("scheduler: tool '%s' not schedulable (task %lld)", event->tool_name,
                  (long long)event->id);
      return -1;
   }

   /* Never execute DANGEROUS tools via scheduler */
   if (meta->capabilities & TOOL_CAP_DANGEROUS) {
      LOG_ERROR("scheduler: refusing to execute dangerous tool '%s' (task %lld)", event->tool_name,
                (long long)event->id);
      return -1;
   }

   /* Check if tool is enabled at runtime */
   if (!tool_registry_is_enabled(event->tool_name)) {
      LOG_WARNING("scheduler: tool '%s' disabled (task %lld)", event->tool_name,
                  (long long)event->id);
      return -1;
   }

   /* Get callback */
   tool_callback_fn callback = meta->callback;
   if (!callback) {
      LOG_WARNING("scheduler: tool '%s' has no callback (task %lld)", event->tool_name,
                  (long long)event->id);
      return -1;
   }

   /* Execute the tool */
   LOG_INFO("scheduler: executing task %lld: %s(%s, %s)", (long long)event->id, event->tool_name,
            event->tool_action, event->tool_value);

   char value_buf[SCHED_TOOL_VALUE_MAX];
   snprintf(value_buf, sizeof(value_buf), "%s", event->tool_value);

   int should_respond = 0;
   char *result = callback(event->tool_action, value_buf, &should_respond);

   if (result) {
      LOG_INFO("scheduler: task %lld result: %.200s", (long long)event->id, result);
      free(result);
   }

   return 0;
}

/* =============================================================================
 * Alarm Sound Playback (separate thread)
 * ============================================================================= */

/** Write volume-scaled PCM to playback stream; returns false if stopped early */
static bool write_scaled_pcm(audio_stream_playback_handle_t *pb,
                             const int16_t *src,
                             size_t samples,
                             float vol_scale) {
   int16_t scaled_buf[1024];
   size_t remaining = samples;
   size_t offset = 0;
   while (remaining > 0 && !alarm_sound_stop) {
      size_t chunk = remaining > 1024 ? 1024 : remaining;
      for (size_t j = 0; j < chunk; j++)
         scaled_buf[j] = (int16_t)(src[offset + j] * vol_scale);
      audio_stream_playback_write(pb, scaled_buf, chunk);
      offset += chunk;
      remaining -= chunk;
   }
   return !alarm_sound_stop;
}

static void *alarm_sound_thread(void *arg) {
   sched_event_t *event = (sched_event_t *)arg;
   bool is_alarm = (event->event_type == SCHED_EVENT_ALARM);

   /* Wait for TTS to complete */
   tts_wait_for_completion(5000);

   alarm_sound_playing = true;
   alarm_sound_stop = false;

   int timeout_sec = g_config.scheduler.alarm_timeout_sec;
   if (timeout_sec > 300)
      timeout_sec = 300;
   time_t sound_start = time(NULL);

   /* Open playback stream for alarm audio */
   audio_stream_params_t params;
   audio_stream_playback_default_params(&params);
   params.sample_rate = DAWN_CHIME_SAMPLE_RATE;
   params.channels = 1;

   /* Apply volume scaling */
   int volume_pct = g_config.scheduler.alarm_volume;
   if (volume_pct <= 0)
      volume_pct = 80;
   if (volume_pct > 100)
      volume_pct = 100;
   float vol_scale = (float)volume_pct / 100.0f;

   audio_hw_params_t hw_params;
   audio_stream_playback_handle_t *pb = audio_stream_playback_open(g_config.audio.playback_device,
                                                                   &params, &hw_params);

   /* Play chime/tone */
   if (is_alarm && alarm_tone_buf.pcm && pb) {
      /* Looping alarm tone until dismissed or timeout */
      while (!alarm_sound_stop && (time(NULL) - sound_start) < timeout_sec) {
         if (!write_scaled_pcm(pb, alarm_tone_buf.pcm, alarm_tone_buf.samples, vol_scale))
            break;
         /* Gap between repetitions */
         size_t gap_samples = (DAWN_CHIME_SAMPLE_RATE * ALARM_GAP_MS) / 1000;
         int16_t silence[512];
         memset(silence, 0, sizeof(silence));
         while (gap_samples > 0 && !alarm_sound_stop) {
            size_t chunk = gap_samples > 512 ? 512 : gap_samples;
            audio_stream_playback_write(pb, silence, chunk);
            gap_samples -= chunk;
         }
      }
   } else if (chime_buf.pcm && pb) {
      /* Single chime for timers/reminders */
      write_scaled_pcm(pb, chime_buf.pcm, chime_buf.samples, vol_scale);
   } else if (!pb) {
      /* Fallback: sleep for approximate duration if audio backend unavailable */
      LOG_WARNING("scheduler: audio playback unavailable, sleeping for chime duration");
      if (is_alarm) {
         while (!alarm_sound_stop && (time(NULL) - sound_start) < timeout_sec)
            usleep(500000);
      } else {
         usleep((unsigned int)(chime_buf.samples * 1000000 / DAWN_CHIME_SAMPLE_RATE));
      }
   }

   if (pb) {
      if (!alarm_sound_stop)
         audio_stream_playback_drain(pb);
      else
         audio_stream_playback_drop(pb);
      audio_stream_playback_close(pb);
   }

   alarm_sound_playing = false;

   /* For non-alarm types (timer, reminder), auto-dismiss after chime */
   if (!is_alarm && !alarm_sound_stop) {
      scheduler_db_update_status(event->id, SCHED_STATUS_DISMISSED);
      pthread_mutex_lock(&ringing_mutex);
      if (ringing_event.id == event->id) {
         alarm_ringing = false;
         memset(&ringing_event, 0, sizeof(ringing_event));
      }
      pthread_mutex_unlock(&ringing_mutex);
      LOG_INFO("scheduler: auto-dismissed %s %lld after chime",
               event->event_type == SCHED_EVENT_TIMER ? "timer" : "reminder", (long long)event->id);

#ifdef ENABLE_WEBUI
      sched_event_t updated;
      if (scheduler_db_get(event->id, &updated) == 0)
         scheduler_broadcast_notification(&updated, "Auto-dismissed");
#endif
   }

   /* If alarm timed out, mark as timed_out */
   if (is_alarm && !alarm_sound_stop) {
      scheduler_db_update_status(event->id, SCHED_STATUS_TIMED_OUT);
      pthread_mutex_lock(&ringing_mutex);
      if (ringing_event.id == event->id) {
         alarm_ringing = false;
         memset(&ringing_event, 0, sizeof(ringing_event));
      }
      pthread_mutex_unlock(&ringing_mutex);
      LOG_INFO("scheduler: alarm %lld timed out after %ds", (long long)event->id, timeout_sec);

#ifdef ENABLE_WEBUI
      /* Notify WebUI of timeout */
      sched_event_t updated;
      if (scheduler_db_get(event->id, &updated) == 0)
         scheduler_broadcast_notification(&updated, "Alarm timed out");
#endif

      /* Schedule next occurrence for recurring alarms */
      schedule_next_occurrence(event);
   }

   free(event);
   return NULL;
}

static void start_alarm_sound(const sched_event_t *event) {
   /* Only one alarm sound at a time */
   if (alarm_sound_playing) {
      alarm_sound_stop = true;
      /* Wait for previous sound thread to finish (up to 2s) */
      for (int i = 0; i < 40 && alarm_sound_playing; i++)
         usleep(50000);
   }

   sched_event_t *event_copy = malloc(sizeof(sched_event_t));
   if (!event_copy)
      return;
   memcpy(event_copy, event, sizeof(sched_event_t));

   pthread_t sound_tid;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

   if (pthread_create(&sound_tid, &attr, alarm_sound_thread, event_copy) != 0) {
      LOG_ERROR("scheduler: failed to create alarm sound thread");
      free(event_copy);
   }
   pthread_attr_destroy(&attr);
}

/* =============================================================================
 * Recurring Event Scheduling
 * ============================================================================= */

/**
 * @brief Check if a day-of-week matches a recurrence pattern
 *
 * @param wday Day of week (0=Sun, 1=Mon, ..., 6=Sat)
 * @param recurrence Recurrence type
 * @param recurrence_days CSV of day names for SCHED_RECUR_CUSTOM (e.g., "mon,wed,fri")
 * @return true if this day matches the pattern
 */
static bool day_matches_recurrence(int wday,
                                   sched_recurrence_t recurrence,
                                   const char *recurrence_days) {
   switch (recurrence) {
      case SCHED_RECUR_DAILY:
         return true;
      case SCHED_RECUR_WEEKDAYS:
         return (wday >= 1 && wday <= 5);
      case SCHED_RECUR_WEEKENDS:
         return (wday == 0 || wday == 6);
      case SCHED_RECUR_WEEKLY:
         return true; /* Same weekday as original */
      case SCHED_RECUR_CUSTOM: {
         if (!recurrence_days || !recurrence_days[0])
            return false;
         static const char *day_names[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
         const char *needle = day_names[wday];
         /* Search for day name in CSV */
         const char *p = recurrence_days;
         while (*p) {
            while (*p == ' ' || *p == ',')
               p++;
            if (strncasecmp(p, needle, 3) == 0) {
               char next = p[3];
               if (next == '\0' || next == ',' || next == ' ')
                  return true;
            }
            while (*p && *p != ',')
               p++;
         }
         return false;
      }
      default:
         return false;
   }
}

/**
 * @brief Calculate next fire time for a recurring event
 *
 * Uses mktime() with tm_isdst = -1 for DST-aware calculation.
 *
 * @param event The event that just fired
 * @return Next fire time, or 0 if no next occurrence (shouldn't happen for recurring)
 */
static time_t calculate_next_recurrence(const sched_event_t *event) {
   if (event->recurrence == SCHED_RECUR_ONCE)
      return 0;

   /* Parse original_time (HH:MM) for the alarm time-of-day.
    * original_time is always in the user's local timezone (any offset suffix
    * from ISO 8601 input is intentionally ignored by sscanf). */
   int hour = 0, minute = 0;
   if (event->original_time[0]) {
      sscanf(event->original_time, "%d:%d", &hour, &minute);
   } else {
      /* Fall back to fire_at time */
      struct tm tm_fire;
      localtime_r(&event->fire_at, &tm_fire);
      hour = tm_fire.tm_hour;
      minute = tm_fire.tm_min;
   }

   /* Start from tomorrow */
   time_t now = time(NULL);
   struct tm tm_next;
   localtime_r(&now, &tm_next);
   tm_next.tm_hour = hour;
   tm_next.tm_min = minute;
   tm_next.tm_sec = 0;
   tm_next.tm_isdst = -1; /* Let mktime() handle DST */

   /* For weekly recurrence, advance exactly 7 days */
   if (event->recurrence == SCHED_RECUR_WEEKLY) {
      tm_next.tm_mday += 7;
      return mktime(&tm_next);
   }

   /* For daily/weekdays/weekends/custom, find next matching day */
   for (int days = 1; days <= 8; days++) {
      tm_next.tm_mday++;
      time_t candidate = mktime(&tm_next);
      if (candidate <= 0)
         continue;

      /* Re-read after mktime (normalizes the date) */
      localtime_r(&candidate, &tm_next);
      tm_next.tm_hour = hour;
      tm_next.tm_min = minute;
      tm_next.tm_sec = 0;
      tm_next.tm_isdst = -1;

      if (day_matches_recurrence(tm_next.tm_wday, event->recurrence, event->recurrence_days))
         return mktime(&tm_next);
   }

   LOG_WARNING("scheduler: could not find next recurrence for event %lld", (long long)event->id);
   return 0;
}

/**
 * @brief Schedule the next occurrence of a recurring event
 *
 * Creates a new pending event with the next fire time.
 * The original event keeps its terminal status (dismissed/timed_out/missed).
 */
static void schedule_next_occurrence(const sched_event_t *fired_event) {
   if (fired_event->recurrence == SCHED_RECUR_ONCE)
      return;

   time_t next_fire = calculate_next_recurrence(fired_event);
   if (next_fire == 0)
      return;

   /* Create new event as a copy with updated fire time */
   sched_event_t next = *fired_event;
   next.id = 0;
   next.status = SCHED_STATUS_PENDING;
   next.fire_at = next_fire;
   next.fired_at = 0;
   next.snooze_count = 0;
   next.snoozed_until = 0;

   int64_t new_id = scheduler_db_insert(&next);
   if (new_id > 0) {
      struct tm tm_next;
      localtime_r(&next_fire, &tm_next);
      LOG_INFO(
          "scheduler: scheduled next recurrence for '%s' at %04d-%02d-%02d %02d:%02d (id=%lld)",
          next.name, tm_next.tm_year + 1900, tm_next.tm_mon + 1, tm_next.tm_mday, tm_next.tm_hour,
          tm_next.tm_min, (long long)new_id);

      scheduler_notify_new_event();
   } else {
      LOG_ERROR("scheduler: failed to insert next recurrence for '%s'", fired_event->name);
   }
}

/* =============================================================================
 * Event Firing
 * ============================================================================= */

static void fire_event(sched_event_t *event) {
   time_t now = time(NULL);

   /* For TASK type events, execute the tool instead of ringing */
   if (event->event_type == SCHED_EVENT_TASK) {
      scheduler_db_update_status_fired(event->id, SCHED_STATUS_RINGING, now);
      event->status = SCHED_STATUS_RINGING;
      event->fired_at = now;

      int rc = scheduler_execute_task(event);
      sched_status_t final_status = (rc == 0) ? SCHED_STATUS_FIRED : SCHED_STATUS_MISSED;
      scheduler_db_update_status(event->id, final_status);
      event->status = final_status;

      /* Announce result (brief chime + TTS) */
      announce_event(event);

#ifdef ENABLE_WEBUI
      char msg[256];
      snprintf(msg, sizeof(msg), "Scheduled task '%s' %s", event->name,
               rc == 0 ? "completed" : "failed");
      scheduler_broadcast_notification(event, msg);
#endif
      return;
   }

   /* Mark as ringing */
   scheduler_db_update_status_fired(event->id, SCHED_STATUS_RINGING, now);
   event->status = SCHED_STATUS_RINGING;
   event->fired_at = now;

   /* Track ringing state */
   pthread_mutex_lock(&ringing_mutex);
   memcpy(&ringing_event, event, sizeof(sched_event_t));
   alarm_ringing = true;
   pthread_mutex_unlock(&ringing_mutex);

   /* Announce and play sound */
   announce_event(event);
   start_alarm_sound(event);

   /* Non-alarm types (timer, reminder) auto-dismiss in alarm_sound_thread */
}

static void fire_due_events(void) {
   sched_event_t events[10];
   int count = scheduler_db_get_due_events(events, 10);

   for (int i = 0; i < count; i++) {
      fire_event(&events[i]);
   }
}

/* =============================================================================
 * Missed Event Recovery
 * ============================================================================= */

static void recover_missed_events(void) {
   sched_event_t events[20];
   int count = scheduler_db_get_missed_events(events, 20);

   if (count == 0)
      return;

   LOG_INFO("scheduler: recovering %d missed events from downtime", count);

   for (int i = 0; i < count; i++) {
      sched_event_t *e = &events[i];

      switch (e->event_type) {
         case SCHED_EVENT_TIMER:
         case SCHED_EVENT_REMINDER:
            /* Fire immediately with "missed" prefix */
            LOG_INFO("scheduler: firing missed %s '%s' (was due at %lld)",
                     sched_event_type_to_str(e->event_type), e->name, (long long)e->fire_at);
            fire_event(e);
            break;

         case SCHED_EVENT_ALARM:
            if (e->recurrence != SCHED_RECUR_ONCE) {
               /* Recurring: skip to next occurrence */
               scheduler_db_update_status(e->id, SCHED_STATUS_MISSED);
               LOG_INFO("scheduler: skipped missed recurring alarm '%s'", e->name);
               schedule_next_occurrence(e);
            } else {
               scheduler_db_update_status(e->id, SCHED_STATUS_MISSED);
               LOG_INFO("scheduler: marked one-shot alarm '%s' as missed", e->name);
            }
            break;

         case SCHED_EVENT_TASK: {
            const scheduler_config_t *sched_cfg = &config_get()->scheduler;
            bool should_execute = (strcmp(sched_cfg->missed_task_policy, "execute") == 0);
            time_t age = time(NULL) - e->fire_at;

            if (should_execute && age <= sched_cfg->missed_task_max_age_sec) {
               LOG_INFO("scheduler: executing missed task '%s' (age=%lds, policy: execute)",
                        e->name, (long)age);
               fire_event(e);
            } else {
               scheduler_db_update_status(e->id, SCHED_STATUS_MISSED);
               if (should_execute) {
                  LOG_INFO("scheduler: skipped missed task '%s' (age=%lds > max %ds)", e->name,
                           (long)age, sched_cfg->missed_task_max_age_sec);
               } else {
                  LOG_INFO("scheduler: skipped missed task '%s' (policy: skip)", e->name);
               }
            }
            break;
         }
      }
   }
}

/* =============================================================================
 * Scheduler Thread
 * ============================================================================= */

static void *scheduler_thread_func(void *arg) {
   (void)arg;

   LOG_INFO("scheduler: thread started");

   /* Wait 30s for subsystem init before recovery */
   for (int i = 0; i < 30 && !scheduler_shutdown_flag; i++)
      sleep(1);

   if (scheduler_shutdown_flag)
      return NULL;

   /* Clean up old events */
   int deleted = scheduler_db_cleanup_old_events(g_config.scheduler.event_retention_days);
   if (deleted > 0)
      LOG_INFO("scheduler: cleaned up %d old events", deleted);

   /* Recover missed events */
   if (g_config.scheduler.missed_event_recovery)
      recover_missed_events();

   /* Main scheduling loop */
   while (!scheduler_shutdown_flag) {
      pthread_mutex_lock(&scheduler_mutex);

      time_t next_fire = scheduler_db_next_fire_time();

      if (next_fire == 0) {
         /* No pending events - sleep indefinitely until notified */
         pthread_cond_wait(&scheduler_cond, &scheduler_mutex);
      } else {
         /* Compute monotonic wakeup time */
         struct timespec mono_now;
         clock_gettime(CLOCK_MONOTONIC, &mono_now);

         time_t wall_now = time(NULL);
         time_t wall_delta = next_fire - wall_now;
         if (wall_delta < 0)
            wall_delta = 0;

         struct timespec wake_ts;
         wake_ts.tv_sec = mono_now.tv_sec + wall_delta;
         wake_ts.tv_nsec = mono_now.tv_nsec;

         pthread_cond_timedwait(&scheduler_cond, &scheduler_mutex, &wake_ts);
      }

      pthread_mutex_unlock(&scheduler_mutex);

      if (scheduler_shutdown_flag)
         break;

      /* Fire any due events */
      fire_due_events();
   }

   LOG_INFO("scheduler: thread exiting");
   return NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int scheduler_init(void) {
   if (!g_config.scheduler.enabled) {
      LOG_INFO("scheduler: disabled in config");
      return 0;
   }

   /* Initialize condvar with CLOCK_MONOTONIC */
   pthread_condattr_t cond_attr;
   pthread_condattr_init(&cond_attr);
   pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
   pthread_cond_init(&scheduler_cond, &cond_attr);
   pthread_condattr_destroy(&cond_attr);

   /* Generate alarm sounds */
   dawn_chime_generate(&chime_buf);
   dawn_alarm_tone_generate(&alarm_tone_buf);

   /* Start scheduler thread */
   scheduler_shutdown_flag = false;
   scheduler_running = true;

   if (pthread_create(&scheduler_thread_id, NULL, scheduler_thread_func, NULL) != 0) {
      LOG_ERROR("scheduler: failed to create thread");
      scheduler_running = false;
      return 1;
   }

   LOG_INFO("scheduler: initialized");
   return 0;
}

void scheduler_shutdown(void) {
   if (!scheduler_running)
      return;

   LOG_INFO("scheduler: shutting down");

   scheduler_shutdown_flag = true;
   alarm_sound_stop = true;

   /* Wake the scheduler thread */
   pthread_mutex_lock(&scheduler_mutex);
   pthread_cond_signal(&scheduler_cond);
   pthread_mutex_unlock(&scheduler_mutex);

   /* Wait for thread to finish */
   pthread_join(scheduler_thread_id, NULL);
   scheduler_running = false;

   /* Wait for alarm sound thread to finish before freeing PCM buffers */
   for (int i = 0; i < 60 && alarm_sound_playing; i++)
      usleep(50000); /* Up to 3s for sound to stop */

   /* Free PCM buffers */
   dawn_chime_free(&chime_buf);
   dawn_chime_free(&alarm_tone_buf);

   pthread_cond_destroy(&scheduler_cond);

   LOG_INFO("scheduler: shutdown complete");
}

void scheduler_notify_new_event(void) {
   if (!scheduler_running)
      return;

   pthread_mutex_lock(&scheduler_mutex);
   pthread_cond_signal(&scheduler_cond);
   pthread_mutex_unlock(&scheduler_mutex);
}

bool scheduler_is_ringing(void) {
   return alarm_ringing;
}

int scheduler_get_ringing(sched_event_t *event) {
   pthread_mutex_lock(&ringing_mutex);
   if (!alarm_ringing) {
      pthread_mutex_unlock(&ringing_mutex);
      return -1;
   }
   memcpy(event, &ringing_event, sizeof(sched_event_t));
   pthread_mutex_unlock(&ringing_mutex);
   return 0;
}

int scheduler_dismiss(int64_t event_id) {
   pthread_mutex_lock(&ringing_mutex);

   if (!alarm_ringing) {
      pthread_mutex_unlock(&ringing_mutex);
      return -1;
   }

   int64_t id = (event_id > 0) ? event_id : ringing_event.id;

   /* Clear ringing state while still holding the lock to prevent
    * alarm_sound_thread from also dismissing */
   alarm_ringing = false;
   memset(&ringing_event, 0, sizeof(ringing_event));
   pthread_mutex_unlock(&ringing_mutex);

   /* Stop alarm sound */
   scheduler_stop_alarm_sound();

   /* Optimistic update: only dismiss if still ringing in DB */
   int result = scheduler_db_dismiss(id);
   if (result == 0) {
      LOG_INFO("scheduler: dismissed event %lld", (long long)id);

      /* Schedule next occurrence for recurring events */
      sched_event_t dismissed;
      if (scheduler_db_get(id, &dismissed) == 0) {
         schedule_next_occurrence(&dismissed);

#ifdef ENABLE_WEBUI
         scheduler_broadcast_notification(&dismissed, "Dismissed");
#endif
      }
   }

   return result;
}

int scheduler_snooze(int64_t event_id, int snooze_minutes) {
   pthread_mutex_lock(&ringing_mutex);

   if (!alarm_ringing) {
      pthread_mutex_unlock(&ringing_mutex);
      return -1;
   }

   int64_t id = (event_id > 0) ? event_id : ringing_event.id;
   int max_snooze = g_config.scheduler.max_snooze_count;
   int current_snooze = ringing_event.snooze_count;

   /* Clear ringing state while holding lock */
   alarm_ringing = false;
   memset(&ringing_event, 0, sizeof(ringing_event));
   pthread_mutex_unlock(&ringing_mutex);

   /* Check max snooze count */
   if (current_snooze >= max_snooze) {
      LOG_WARNING("scheduler: max snooze count (%d) reached for event %lld", max_snooze,
                  (long long)id);
      /* Already cleared ringing state, just dismiss in DB */
      scheduler_stop_alarm_sound();
      scheduler_db_dismiss(id);
      LOG_INFO("scheduler: dismissed event %lld (max snooze reached)", (long long)id);

#ifdef ENABLE_WEBUI
      sched_event_t updated;
      if (scheduler_db_get(id, &updated) == 0)
         scheduler_broadcast_notification(&updated, "Dismissed (max snooze reached)");
#endif
      return 0;
   }

   /* Stop alarm sound */
   scheduler_stop_alarm_sound();

   if (snooze_minutes <= 0)
      snooze_minutes = g_config.scheduler.default_snooze_minutes;
   if (snooze_minutes > 120)
      snooze_minutes = 120;

   time_t new_fire = time(NULL) + snooze_minutes * 60;
   int result = scheduler_db_snooze(id, new_fire);

   if (result == 0) {
      /* Wake scheduler to recalculate next fire time */
      scheduler_notify_new_event();
      LOG_INFO("scheduler: snoozed event %lld for %d minutes", (long long)id, snooze_minutes);

#ifdef ENABLE_WEBUI
      sched_event_t updated;
      if (scheduler_db_get(id, &updated) == 0) {
         char msg[64];
         snprintf(msg, sizeof(msg), "Snoozed for %d minute%s", snooze_minutes,
                  snooze_minutes == 1 ? "" : "s");
         scheduler_broadcast_notification(&updated, msg);
      }
#endif
   }

   return result;
}

void scheduler_stop_alarm_sound(void) {
   alarm_sound_stop = true;
}
