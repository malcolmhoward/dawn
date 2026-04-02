/**
 * DAWN Scheduler Notifications
 * Handles alarm/timer/reminder notifications from the daemon scheduler.
 * Displays a notification banner with dismiss/snooze buttons.
 */
(function () {
   'use strict';

   let container = null;
   let chimeCtx = null; // Web Audio context for alarm chime
   const localDismissed = new Set(); // event IDs dismissed locally (suppress rebroadcast)
   function unreadKey() {
      const user =
         typeof DawnState !== 'undefined' && DawnState.authState
            ? DawnState.authState.username
            : '';
      return 'dawn_unread_briefings' + (user ? '_' + user : '');
   }

   function getUnreadBriefings() {
      try {
         return JSON.parse(localStorage.getItem(unreadKey()) || '[]');
      } catch {
         return [];
      }
   }

   function addUnreadBriefing(convId) {
      if (!convId) return;
      const list = getUnreadBriefings();
      if (!list.includes(convId)) {
         list.push(convId);
         localStorage.setItem(unreadKey(), JSON.stringify(list));
      }
   }

   function removeUnreadBriefing(convId) {
      const list = getUnreadBriefings().filter((id) => id !== convId);
      localStorage.setItem(unreadKey(), JSON.stringify(list));
      /* Update sidebar if available */
      if (typeof DawnHistory !== 'undefined' && DawnHistory.refreshUnread) {
         DawnHistory.refreshUnread();
      }
   }

   /**
    * Ensure notification container exists
    */
   function ensureContainer() {
      if (container) return container;
      container = document.createElement('div');
      container.id = 'scheduler-notifications';
      document.body.appendChild(container);
      return container;
   }

   /**
    * Play a browser alarm chime using Web Audio API
    */
   function playChime(eventType) {
      try {
         if (!chimeCtx) {
            chimeCtx = new (window.AudioContext || window.webkitAudioContext)();
         }
         /* Resume suspended context (browser autoplay policy) */
         if (chimeCtx.state === 'suspended') {
            chimeCtx.resume();
         }

         const duration = eventType === 'alarm' ? 0.6 : 0.3;
         const freq = eventType === 'alarm' ? 880 : eventType === 'reminder' ? 660 : 523;

         const osc = chimeCtx.createOscillator();
         const gain = chimeCtx.createGain();
         osc.connect(gain);
         gain.connect(chimeCtx.destination);
         osc.frequency.value = freq;
         osc.type = 'sine';
         gain.gain.setValueAtTime(0.3, chimeCtx.currentTime);
         gain.gain.exponentialRampToValueAtTime(0.001, chimeCtx.currentTime + duration);
         osc.start(chimeCtx.currentTime);
         osc.stop(chimeCtx.currentTime + duration);
      } catch (e) {
         console.warn('Scheduler: Could not play chime:', e);
      }
   }

   /**
    * Get color class for event type
    */
   function typeClass(eventType) {
      switch (eventType) {
         case 'alarm':
            return 'sched-alarm';
         case 'reminder':
            return 'sched-reminder';
         case 'timer':
            return 'sched-timer';
         case 'task':
            return 'sched-task';
         case 'briefing':
            return 'sched-briefing';
         default:
            return 'sched-timer';
      }
   }

   /**
    * Get display label for event type
    */
   function typeLabel(eventType) {
      switch (eventType) {
         case 'alarm':
            return 'ALARM';
         case 'reminder':
            return 'REMINDER';
         case 'timer':
            return 'TIMER';
         case 'task':
            return 'TASK';
         case 'briefing':
            return 'BRIEFING';
         default:
            return 'EVENT';
      }
   }

   /**
    * Send scheduler action to daemon
    */
   function sendAction(action, eventId, snoozeMinutes) {
      const payload = { action, event_id: eventId };
      if (snoozeMinutes) payload.snooze_minutes = snoozeMinutes;
      DawnWS.send({ type: 'scheduler_action', payload });
   }

   /**
    * Create and show a notification banner
    */
   function showNotification(payload) {
      const el = ensureContainer();
      const eventId = payload.event_id;
      const eventType = payload.event_type || 'timer';
      const status = payload.status || 'ringing';
      const name = payload.name || '';
      const message = payload.message || name;

      // Remove existing notification for same event
      const existing = el.querySelector(`[data-event-id="${eventId}"]`);
      if (existing) existing.remove();

      const isRinging = status === 'ringing';

      const banner = document.createElement('div');
      const ringingClass = isRinging ? ' sched-ringing' : '';
      banner.className = `sched-banner ${typeClass(eventType)}${ringingClass}`;
      banner.dataset.eventId = eventId;
      banner.setAttribute('role', 'alertdialog');
      banner.setAttribute('aria-live', 'assertive');
      banner.setAttribute('aria-label', `${typeLabel(eventType)}: ${message}`);

      // Only alarms support snooze (timers/reminders auto-dismiss on daemon)
      const canSnooze = isRinging && eventType === 'alarm';

      const isBriefing = eventType === 'briefing';
      const conversationId = payload.conversation_id;

      let actionsHtml;
      if (isBriefing && conversationId) {
         actionsHtml = `<div class="sched-actions">
               <button class="sched-btn sched-btn-view" title="View">View</button>
               <button class="sched-btn sched-btn-dismiss" title="Close">Close</button>
            </div>`;
      } else if (isRinging) {
         actionsHtml = `<div class="sched-actions">
               ${canSnooze ? '<button class="sched-btn sched-btn-snooze" title="Snooze">Snooze</button>' : ''}
               <button class="sched-btn sched-btn-dismiss" title="Dismiss">Dismiss</button>
            </div>`;
      } else {
         actionsHtml = `<div class="sched-actions">
               <button class="sched-btn sched-btn-dismiss" title="Close">Close</button>
            </div>`;
      }

      banner.innerHTML = `
         <div class="sched-banner-content">
            <span class="sched-type-badge">${typeLabel(eventType)}</span>
            <span class="sched-message">${escapeHtml(message)}</span>
         </div>
         ${actionsHtml}
      `;

      // Wire buttons
      const dismissBtn = banner.querySelector('.sched-btn-dismiss');
      if (dismissBtn) {
         dismissBtn.addEventListener('click', () => {
            localDismissed.add(eventId);
            setTimeout(() => localDismissed.delete(eventId), 5000);
            sendAction('dismiss', eventId);
            /* Stop TTS if scheduler audio is playing */
            if (typeof DawnAudioPlayback !== 'undefined' && DawnAudioPlayback.stop) {
               DawnAudioPlayback.stop();
            }
            banner.classList.add('sched-banner-out');
            setTimeout(() => banner.remove(), 300);
         });
      }

      const snoozeBtn = banner.querySelector('.sched-btn-snooze');
      if (snoozeBtn) {
         snoozeBtn.addEventListener('click', () => {
            sendAction('snooze', eventId, 0);
            banner.classList.add('sched-banner-out');
            setTimeout(() => banner.remove(), 300);
         });
      }

      const viewBtn = banner.querySelector('.sched-btn-view');
      if (viewBtn) {
         viewBtn.addEventListener('click', () => {
            if (typeof DawnHistory !== 'undefined' && conversationId) {
               DawnHistory.loadConversation(conversationId);
               DawnHistory.open();
               removeUnreadBriefing(conversationId);
            }
            banner.classList.add('sched-banner-out');
            setTimeout(() => banner.remove(), 300);
         });
      }

      // Keyboard: Escape to dismiss
      banner.tabIndex = 0;
      banner.addEventListener('keydown', (e) => {
         if (e.key === 'Escape') {
            dismissBtn.click();
         }
      });

      el.appendChild(banner);
      banner.focus();

      // Auto-dismiss non-ringing notifications (briefings: 60s, others: 10s)
      if (!isRinging || isBriefing) {
         const dismissMs = isBriefing ? 60000 : 10000;
         setTimeout(() => {
            if (banner.parentNode) {
               banner.classList.add('sched-banner-out');
               setTimeout(() => banner.remove(), 300);
            }
         }, dismissMs);
      }
   }

   /**
    * Simple HTML escape
    */
   function escapeHtml(str) {
      if (typeof DawnFormat !== 'undefined' && DawnFormat.escapeHtml) {
         return DawnFormat.escapeHtml(str);
      }
      const div = document.createElement('div');
      div.textContent = str;
      return div.innerHTML;
   }

   /**
    * Handle scheduler notification from WebSocket
    */
   function handleNotification(payload) {
      if (!payload) return;

      /* Suppress rebroadcast of our own dismiss action */
      if (payload.status !== 'ringing' && localDismissed.has(payload.event_id)) {
         return;
      }

      /* Ignore timed_out / auto-dismiss if banner is already showing —
       * keep it up until user explicitly dismisses */
      if (payload.status === 'timed_out' || payload.message?.startsWith('Auto-')) {
         const el = document.getElementById('scheduler-notifications');
         if (el?.querySelector(`[data-event-id="${payload.event_id}"]`)) {
            return;
         }
      }

      console.log('Scheduler notification:', payload);
      showNotification(payload);
      /* Skip chime if TTS audio is being routed to this client */
      if (!payload.tts_routed) {
         playChime(payload.event_type);
      }

      /* Track unread briefings and refresh history sidebar */
      if (payload.event_type === 'briefing' && payload.conversation_id) {
         addUnreadBriefing(payload.conversation_id);
         if (typeof DawnHistory !== 'undefined') {
            DawnHistory.refreshList();
            DawnHistory.open();
         }
      }
   }

   // Public API
   window.DawnScheduler = {
      handleNotification,
      getUnreadBriefings,
      removeUnreadBriefing,
   };
})();
