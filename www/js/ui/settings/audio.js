/**
 * DAWN Settings - Audio Device Management
 * Audio device discovery and selection functionality
 */
(function () {
   'use strict';

   // Audio device state
   let audioDevicesCache = { capture: [], playback: [] };

   // Callback for setting changes (injected by main settings module)
   let handleSettingChange = null;

   /**
    * Set the callback for handling setting changes
    * @param {Function} callback - Function to call when a setting changes
    */
   function setHandleSettingChange(callback) {
      handleSettingChange = callback;
   }

   /**
    * Request audio devices from server for a specific backend
    * @param {string} backend - Audio backend ('pulse' or 'alsa')
    */
   function requestAudioDevices(backend) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         return;
      }

      DawnWS.send({
         type: 'get_audio_devices',
         payload: { backend: backend },
      });
   }

   /**
    * Handle audio devices response from server
    * @param {Object} payload - Response payload with capture_devices and playback_devices
    */
   function handleGetAudioDevicesResponse(payload) {
      audioDevicesCache.capture = payload.capture_devices || [];
      audioDevicesCache.playback = payload.playback_devices || [];

      // Update capture device field
      updateAudioDeviceField('capture_device', audioDevicesCache.capture);
      updateAudioDeviceField('playback_device', audioDevicesCache.playback);
   }

   /**
    * Update an audio device field from text input to select dropdown
    * @param {string} fieldName - Field name ('capture_device' or 'playback_device')
    * @param {Array} devices - Array of device objects or strings
    */
   function updateAudioDeviceField(fieldName, devices) {
      // ID matches how createSettingField generates it (underscores preserved)
      const inputId = `setting-audio-${fieldName}`;
      const input = document.getElementById(inputId);
      if (!input) return;

      const currentValue = input.value || '';
      const dataKey = input.dataset.key;
      const parent = input.parentElement;

      // Replace input with select
      const select = document.createElement('select');
      select.id = inputId;
      select.dataset.key = dataKey;

      // Add default option
      const defaultOpt = document.createElement('option');
      defaultOpt.value = '';
      defaultOpt.textContent = 'Default';
      select.appendChild(defaultOpt);

      // Add device options (devices can be strings or {id, name} objects)
      devices.forEach((device) => {
         const opt = document.createElement('option');
         // Handle both string devices and object devices
         const deviceId = typeof device === 'string' ? device : device.id;
         const deviceName = typeof device === 'string' ? device : device.name || device.id;
         opt.value = deviceId;
         opt.textContent = deviceName;
         if (deviceId === currentValue) {
            opt.selected = true;
         }
         select.appendChild(opt);
      });

      // Add change listener
      if (handleSettingChange) {
         select.addEventListener('change', () => handleSettingChange(dataKey, select));
      }

      // Replace input with select
      parent.replaceChild(select, input);
   }

   /**
    * Update audio device fields based on backend selection
    * @param {string} backend - Selected backend ('auto', 'pulse', or 'alsa')
    */
   function updateAudioBackendState(backend) {
      // IDs match how createSettingField generates them (underscores preserved)
      const captureInput = document.getElementById('setting-audio-capture_device');
      const playbackInput = document.getElementById('setting-audio-playback_device');

      if (backend === 'auto') {
         // Grey out device fields when auto
         if (captureInput) {
            captureInput.disabled = true;
            captureInput.title = 'Device is auto-detected when backend is "auto"';
         }
         if (playbackInput) {
            playbackInput.disabled = true;
            playbackInput.title = 'Device is auto-detected when backend is "auto"';
         }
      } else {
         // Enable device fields and request available devices
         if (captureInput) {
            captureInput.disabled = false;
            captureInput.title = '';
         }
         if (playbackInput) {
            playbackInput.disabled = false;
            playbackInput.title = '';
         }

         // Request available devices for this backend
         requestAudioDevices(backend);
      }
   }

   /**
    * Toggle visibility of a secret/password input field
    * @param {string} targetId - ID of the input element to toggle
    */
   function toggleSecretVisibility(targetId) {
      const input = document.getElementById(targetId);
      if (!input) return;

      input.type = input.type === 'password' ? 'text' : 'password';
   }

   /**
    * Get the current audio devices cache
    * @returns {Object} Object with capture and playback arrays
    */
   function getAudioDevicesCache() {
      return audioDevicesCache;
   }

   // Export for settings module
   window.DawnSettingsAudio = {
      setHandleSettingChange,
      requestAudioDevices,
      handleGetAudioDevicesResponse,
      updateAudioDeviceField,
      updateAudioBackendState,
      toggleSecretVisibility,
      getAudioDevicesCache,
   };
})();
