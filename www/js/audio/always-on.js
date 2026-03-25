/**
 * DAWN Always-On Voice Mode — Browser Coordinator
 *
 * Two-step UX: dropdown selects the MODE, mic button activates it.
 * - "Hold to Talk" mode: mic button works as push-to-talk (existing behavior)
 * - "Continuous Listening" mode: mic button click toggles continuous on/off
 *
 * Server drives the always-on state machine. Client responsibilities:
 * - Start/stop continuous audio capture on button toggle
 * - Mute streaming during TTS (echo prevention)
 * - Update UI on state changes
 * - ARIA announcements for accessibility
 * - Error recovery (mic revocation, disconnect, tab return)
 */
(function (global) {
   'use strict';

   const STORAGE_KEY = 'dawn_always_on_mode';

   let selectedMode = 'push-to-talk'; // Mode selected in dropdown
   let enabled = false; // Whether continuous listening is currently active
   let state = 'disabled'; // Server state
   let pendingUnmute = false; // Defer unmute until TTS playback finishes
   let pendingVisualState = null; // Defer visual state update until TTS finishes

   /**
    * Initialize always-on module
    */
   function init() {
      setupDropdown();

      // Restore mode preference from localStorage (mode only, not activation)
      const stored = localStorage.getItem(STORAGE_KEY);
      if (stored === 'continuous') {
         selectedMode = 'continuous';
         updateDropdownActive('continuous');
         updateMicButtonLabel();
      }

      document.addEventListener('visibilitychange', handleVisibilityChange);
   }

   /**
    * Set up the split button dropdown for mode selection.
    * The dropdown only selects the mode — it does NOT activate/deactivate.
    */
   function setupDropdown() {
      const dropdownBtn = document.getElementById('mic-dropdown-btn');
      const dropdown = document.getElementById('mic-dropdown');

      if (!dropdownBtn || !dropdown) {
         return;
      }

      // Toggle dropdown on chevron click
      dropdownBtn.addEventListener('click', function (e) {
         e.preventDefault();
         e.stopPropagation();
         const isOpen = !dropdown.classList.contains('hidden');
         if (isOpen) {
            closeDropdown();
         } else {
            openDropdown();
         }
      });

      // Mode selection — sets preference only, does not activate
      dropdown.addEventListener('click', function (e) {
         if (dropdown.classList.contains('hidden')) return;

         const item = e.target.closest('.dropdown-item');
         if (!item) return;

         const mode = item.dataset.mode;

         // If switching away from continuous while it's active, disable first
         if (mode !== 'continuous' && enabled) {
            disable();
         }

         selectedMode = mode;
         localStorage.setItem(STORAGE_KEY, mode);
         updateDropdownActive(mode);
         updateMicButtonLabel();
         closeDropdown();

         announce(
            mode === 'continuous'
               ? 'Continuous listening mode selected. Press mic button to start.'
               : 'Hold to talk mode selected.'
         );
      });

      // Close dropdown on outside click
      document.addEventListener('click', function (e) {
         if (!e.target.closest('.mic-btn-wrapper')) {
            closeDropdown();
         }
      });

      // Keyboard navigation
      dropdownBtn.addEventListener('keydown', function (e) {
         if (e.key === 'Escape') {
            closeDropdown();
            dropdownBtn.focus();
         } else if (e.key === 'ArrowDown' || e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            if (dropdown.classList.contains('hidden')) {
               openDropdown();
            }
            const firstItem = dropdown.querySelector('.dropdown-item');
            if (firstItem) firstItem.focus();
         }
      });

      dropdown.addEventListener('keydown', function (e) {
         const items = [...dropdown.querySelectorAll('.dropdown-item')];
         const idx = items.indexOf(document.activeElement);

         if (e.key === 'ArrowDown') {
            e.preventDefault();
            const next = items[(idx + 1) % items.length];
            if (next) next.focus();
         } else if (e.key === 'ArrowUp') {
            e.preventDefault();
            const prev = items[(idx - 1 + items.length) % items.length];
            if (prev) prev.focus();
         } else if (e.key === 'Escape') {
            closeDropdown();
            dropdownBtn.focus();
         } else if (e.key === 'Home') {
            e.preventDefault();
            if (items[0]) items[0].focus();
         } else if (e.key === 'End') {
            e.preventDefault();
            if (items[items.length - 1]) items[items.length - 1].focus();
         }
      });
   }

   function closeDropdown() {
      const dropdown = document.getElementById('mic-dropdown');
      const btn = document.getElementById('mic-dropdown-btn');
      if (dropdown) {
         dropdown.classList.add('hidden');
         dropdown.style.pointerEvents = 'none';
      }
      if (btn) btn.setAttribute('aria-expanded', 'false');
   }

   function openDropdown() {
      const dropdown = document.getElementById('mic-dropdown');
      const btn = document.getElementById('mic-dropdown-btn');
      if (dropdown) {
         dropdown.classList.remove('hidden');
         dropdown.style.pointerEvents = '';
      }
      if (btn) btn.setAttribute('aria-expanded', 'true');
   }

   /**
    * Toggle continuous listening (called from mic button click in continuous mode)
    */
   function toggle() {
      if (enabled) {
         disable();
      } else {
         enable();
      }
   }

   /**
    * Enable always-on continuous listening
    */
   function enable() {
      if (enabled) return;

      if (!window.isSecureContext) {
         console.error('Always-on requires HTTPS');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Always-on requires HTTPS', 'error');
         }
         return;
      }

      if (DawnState.getIsRecording()) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Finish recording before enabling continuous mode', 'warning');
         }
         return;
      }

      // Start audio capture before notifying server — mic acquisition is async
      // and may prompt user. If it fails, server gets no audio and auto-disables.
      DawnAudioCapture.startContinuous();

      var sampleRate = DawnAudioCapture.getAudioContextSampleRate
         ? DawnAudioCapture.getAudioContextSampleRate()
         : 48000;

      DawnWS.send({
         type: 'always_on_enable',
         payload: { sample_rate: sampleRate },
      });

      enabled = true;
      updateMicButton();
      updateMicButtonLabel();
      announce('Continuous listening started');
   }

   /**
    * Disable always-on mode
    */
   function disable() {
      if (!enabled) return;

      DawnWS.send({ type: 'always_on_disable' });
      DawnAudioCapture.stopContinuous();

      enabled = false;
      state = 'disabled';
      pendingUnmute = false;
      pendingVisualState = null;
      updateMicButton();
      updateMicButtonLabel();
      announce('Continuous listening stopped');
   }

   /**
    * Handle incoming always-on messages from server
    */
   function handleMessage(msg) {
      switch (msg.type) {
         case 'always_on_state':
            onStateChange(msg.payload.state);
            break;
         case 'wake_detected':
            console.log('Wake word detected:', msg.payload.transcript);
            break;
         case 'recording_end':
            break;
         case 'always_on_error':
            handleError(msg.payload);
            break;
      }
   }

   /**
    * Handle state change from server
    */
   function onStateChange(newState) {
      var oldState = state;
      state = newState;

      // TTS echo prevention: mute during processing, unmute on listening
      if (newState === 'processing' && DawnAudioCapture.muteContinuous) {
         DawnAudioCapture.muteContinuous();
         pendingUnmute = false;
         pendingVisualState = null;
      } else if (
         newState === 'listening' &&
         oldState === 'processing' &&
         DawnAudioCapture.unmuteContinuous
      ) {
         // Defer unmute AND visual state until TTS playback finishes
         if (typeof DawnAudioPlayback !== 'undefined' && DawnAudioPlayback.isPlaying()) {
            pendingUnmute = true;
            pendingVisualState = newState;
            console.log('Always-on: deferring unmute + visual until TTS playback finishes');
            // Don't update visuals yet — keep showing processing/speaking state
            return;
         } else {
            DawnAudioCapture.unmuteContinuous();
         }
      }

      // Handle server-side disable (e.g., auto-disable on no audio)
      if (newState === 'disabled' && enabled) {
         DawnAudioCapture.stopContinuous();
         enabled = false;
      }

      updateMicButton();
      updateMicButtonLabel();
      updateVisualizer();
      announce(stateAnnouncement(newState));
   }

   /**
    * Handle error from server
    */
   function handleError(payload) {
      console.error('Always-on error:', payload);

      if (payload.code === 'ALREADY_ACTIVE') {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Always-on is active in another tab', 'warning');
         }
      } else {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.message || 'Always-on error', 'error');
         }
      }

      // Reset local state on any error
      enabled = false;
      state = 'disabled';
      DawnAudioCapture.stopContinuous();
      updateMicButton();
      updateMicButtonLabel();
   }

   /**
    * Update the dropdown active indicator
    */
   function updateDropdownActive(mode) {
      var dropdown = document.getElementById('mic-dropdown');
      if (!dropdown) return;

      dropdown.querySelectorAll('.dropdown-item').forEach(function (item) {
         item.classList.toggle('active', item.dataset.mode === mode);
      });
   }

   /**
    * Update mic button label/title based on selected mode and active state
    */
   function updateMicButtonLabel() {
      var btn = DawnElements.micBtn;
      if (!btn) return;

      var label;
      if (selectedMode === 'continuous') {
         if (enabled) {
            label = 'Stop continuous listening';
            btn.textContent = 'Stop';
         } else {
            label = 'Start continuous listening';
            btn.textContent = 'Listen';
         }
      } else {
         label = 'Hold to speak';
         btn.textContent = 'Mic';
      }
      btn.title = label;
      btn.setAttribute('aria-label', label);
   }

   /**
    * Update mic button visual state based on always-on state
    */
   function updateMicButton() {
      var wrapper = document.querySelector('.mic-btn-wrapper');
      if (!wrapper) return;

      wrapper.classList.remove(
         'always-on-listening',
         'always-on-wake-check',
         'always-on-recording',
         'always-on-processing'
      );

      if (!enabled) return;

      switch (state) {
         case 'listening':
            wrapper.classList.add('always-on-listening');
            break;
         case 'wake_check':
         case 'wake_pending':
            wrapper.classList.add('always-on-wake-check');
            break;
         case 'recording':
            wrapper.classList.add('always-on-recording');
            break;
         case 'processing':
            wrapper.classList.add('always-on-processing');
            break;
      }
   }

   /**
    * Update status dot and text to reflect always-on state.
    * Overrides the normal "IDLE" display with always-on state info.
    * Pass null to restore normal display.
    */
   function updateStatusIndicator(alwaysOnState) {
      var dot = DawnElements.statusDot;
      var text = DawnElements.statusText;
      var miniDot = DawnElements.miniStatusDot;
      var miniText = DawnElements.miniStatusText;
      if (!dot || !text) return;

      if (!alwaysOnState || alwaysOnState === 'disabled') {
         // Restore to current app state
         var appState = DawnState.getAppState() || 'idle';
         dot.className = appState;
         text.textContent = appState.toUpperCase();
         if (miniDot) miniDot.className = 'status-dot ' + appState;
         if (miniText) miniText.textContent = appState.toUpperCase();
         return;
      }

      var statusMap = {
         listening: { css: 'listening', label: 'LISTENING' },
         wake_check: { css: 'listening', label: 'LISTENING' },
         wake_pending: { css: 'thinking', label: 'CHECKING' },
         recording: { css: 'listening', label: 'RECORDING' },
         processing: { css: 'thinking', label: 'THINKING' },
      };

      var info = statusMap[alwaysOnState];
      if (!info) return;

      dot.className = info.css;
      text.textContent = info.label;
      if (miniDot) miniDot.className = 'status-dot ' + info.css;
      if (miniText) miniText.textContent = info.label;
   }

   /**
    * Update visualizer ring state
    */
   function updateVisualizer() {
      var ring = document.getElementById('ring-container');
      if (!ring) return;

      ring.classList.remove(
         'always-on-listening',
         'always-on-wake-check',
         'always-on-recording',
         'always-on-processing'
      );

      if (!enabled) {
         // Stop input visualization when disabled
         if (typeof DawnVisualization !== 'undefined' && DawnVisualization.stopInputFFT) {
            DawnVisualization.stopInputFFT();
         }
         // Restore normal status display
         updateStatusIndicator(null);
         return;
      }

      // Start/stop input FFT visualization based on state
      if (typeof DawnVisualization !== 'undefined') {
         if (
            state === 'listening' ||
            state === 'wake_check' ||
            state === 'wake_pending' ||
            state === 'recording'
         ) {
            if (DawnVisualization.startInputFFT) {
               DawnVisualization.startInputFFT();
            }
         } else {
            if (DawnVisualization.stopInputFFT) {
               DawnVisualization.stopInputFFT();
            }
         }
      }

      // Update status text/dot to reflect always-on state
      updateStatusIndicator(state);

      // Remove idle dimming — always-on ring should stay bright
      ring.classList.remove('idle');

      switch (state) {
         case 'listening':
            ring.classList.add('always-on-listening');
            break;
         case 'wake_check':
         case 'wake_pending':
            ring.classList.add('always-on-wake-check');
            break;
         case 'recording':
            ring.classList.add('always-on-recording');
            break;
         case 'processing':
            ring.classList.add('always-on-processing');
            break;
      }
   }

   /**
    * ARIA announcement for state changes
    */
   function announce(message) {
      if (!message) return;
      var announcer = document.getElementById('always-on-announcer');
      if (!announcer) {
         announcer = document.createElement('div');
         announcer.id = 'always-on-announcer';
         announcer.setAttribute('aria-live', 'polite');
         announcer.setAttribute('aria-atomic', 'true');
         announcer.className = 'sr-only';
         document.body.appendChild(announcer);
      }
      announcer.textContent = message;
   }

   function stateAnnouncement(s) {
      var messages = {
         listening: 'Listening for wake word',
         wake_check: 'Speech detected, checking for wake word',
         wake_pending: 'Checking for wake word',
         recording: 'Wake word detected, recording your command',
         processing: 'Processing your request',
         disabled: 'Continuous listening stopped',
      };
      return messages[s] || '';
   }

   function handleVisibilityChange() {
      if (!document.hidden && enabled) {
         DawnWS.send({ type: 'always_on_state_request' });
      }
   }

   function handleReconnect() {
      // Server-side always_on_ctx is destroyed on disconnect. Reset client state
      // so the UI doesn't show active when the server has no context.
      if (enabled) {
         DawnAudioCapture.stopContinuous();
         enabled = false;
         state = 'disabled';
         pendingUnmute = false;
         updateMicButton();
         updateMicButtonLabel();
      }
   }

   function handleMicRevoked() {
      if (enabled) {
         console.warn('Always-on: mic permission revoked, disabling');
         enabled = false;
         state = 'disabled';
         DawnWS.send({ type: 'always_on_disable' });
         updateMicButton();
         updateMicButtonLabel();
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Microphone access lost — continuous listening disabled', 'warning');
         }
      }
   }

   /**
    * Check if continuous mode is selected (for mic button behavior in dawn.js)
    */
   function isContinuousMode() {
      return selectedMode === 'continuous';
   }

   function isEnabled() {
      return enabled;
   }

   function getState() {
      return state;
   }

   /**
    * Called when TTS playback finishes. Completes deferred unmute if pending.
    */
   function onPlaybackEnd() {
      if (pendingUnmute) {
         pendingUnmute = false;
         if (enabled && DawnAudioCapture.unmuteContinuous) {
            console.log('Always-on: TTS finished, unmuting now');
            DawnAudioCapture.unmuteContinuous();
         }
      }
      if (pendingVisualState) {
         var deferred = pendingVisualState;
         pendingVisualState = null;
         console.log('Always-on: TTS finished, applying deferred state:', deferred);
         updateMicButton();
         updateMicButtonLabel();
         updateVisualizer();
         announce(stateAnnouncement(deferred));
      }
   }

   global.DawnAlwaysOn = {
      init: init,
      enable: enable,
      disable: disable,
      toggle: toggle,
      handleMessage: handleMessage,
      handleReconnect: handleReconnect,
      handleMicRevoked: handleMicRevoked,
      onPlaybackEnd: onPlaybackEnd,
      isContinuousMode: isContinuousMode,
      isEnabled: isEnabled,
      getState: getState,
   };
})(window);
