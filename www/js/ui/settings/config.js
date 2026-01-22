/**
 * DAWN Settings - Configuration Management
 * Config save/load, persistence, and secrets management
 */
(function () {
   'use strict';

   // Configuration state
   let currentConfig = null;
   let currentSecrets = null;
   let restartRequiredFields = [];
   let changedFields = new Set();
   let dynamicOptions = { asr_models: [], tts_voices: [], bind_addresses: [] };

   // DOM element references (injected by main settings module)
   let settingsElements = {};

   // Callbacks for external dependencies
   let callbacks = {
      renderSettingsSections: null,
      updateSecretsStatusUI: null,
      updateAudioBackendState: null,
      updateCloudModelLists: null,
      extractGlobalDefaults: null,
      applyGlobalDefaultsToControls: null,
      updateLlmControls: null,
      setAuthState: null,
      updateAuthVisibility: null,
      showConfirmModal: null,
      showRestartConfirmation: null,
   };

   /**
    * Set DOM element references
    * @param {Object} elements - Object containing DOM element references
    */
   function setElements(elements) {
      settingsElements = elements;
   }

   /**
    * Set callbacks for external dependencies
    * @param {Object} cbs - Object containing callback functions
    */
   function setCallbacks(cbs) {
      Object.assign(callbacks, cbs);
   }

   /**
    * Get current config
    * @returns {Object|null} Current config object
    */
   function getCurrentConfig() {
      return currentConfig;
   }

   /**
    * Get current secrets
    * @returns {Object|null} Current secrets object
    */
   function getCurrentSecrets() {
      return currentSecrets;
   }

   /**
    * Get restart required fields array
    * @returns {Array} Array of field keys that require restart
    */
   function getRestartRequiredFields() {
      return restartRequiredFields;
   }

   /**
    * Get changed fields set
    * @returns {Set} Set of changed field keys
    */
   function getChangedFields() {
      return changedFields;
   }

   /**
    * Clear changed fields tracking
    */
   function clearChangedFields() {
      changedFields.clear();
   }

   /**
    * Mark a field as changed
    * @param {string} key - Field key
    */
   function markFieldChanged(key) {
      changedFields.add(key);
   }

   /**
    * Get dynamic options for select fields
    * @returns {Object} Dynamic options object
    */
   function getDynamicOptions() {
      return dynamicOptions;
   }

   /**
    * Request config from server
    */
   function requestConfig() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         return;
      }

      DawnWS.send({ type: 'get_config' });
   }

   /**
    * Request models list from server
    */
   function requestModelsList() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         return;
      }

      DawnWS.send({ type: 'list_models' });
   }

   /**
    * Request network interfaces list from server
    */
   function requestInterfacesList() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         return;
      }

      DawnWS.send({ type: 'list_interfaces' });
   }

   /**
    * Handle models list response from server
    * @param {Object} payload - Response payload
    */
   function handleModelsListResponse(payload) {
      if (payload.asr_models) {
         dynamicOptions.asr_models = payload.asr_models;
      }
      if (payload.tts_voices) {
         dynamicOptions.tts_voices = payload.tts_voices;
      }
      // Update any already-rendered dynamic selects
      updateDynamicSelects();
   }

   /**
    * Handle interfaces list response from server
    * @param {Object} payload - Response payload
    */
   function handleInterfacesListResponse(payload) {
      if (payload.addresses) {
         dynamicOptions.bind_addresses = payload.addresses;
      }

      // Update any already-rendered dynamic selects
      updateDynamicSelects();
   }

   /**
    * Update dynamic select dropdowns with server-provided options
    */
   function updateDynamicSelects() {
      document.querySelectorAll('select[data-dynamic-key]').forEach((select) => {
         const key = select.dataset.dynamicKey;
         const options = dynamicOptions[key] || [];
         const currentValue = select.value;

         // Clear existing options except the first (current value placeholder)
         while (select.options.length > 1) {
            select.remove(1);
         }

         // Add options from server (skip current value to avoid duplicate)
         options.forEach((opt) => {
            if (opt === currentValue) return; // Skip - already in first option
            const option = document.createElement('option');
            option.value = opt;
            option.textContent = opt;
            select.appendChild(option);
         });

         // If current value isn't in options, mark it as "(current)"
         if (currentValue && !options.includes(currentValue)) {
            select.options[0].textContent = currentValue + ' (current)';
         }
      });
   }

   /**
    * Handle get_config response from server
    * @param {Object} payload - Response payload
    */
   function handleGetConfigResponse(payload) {
      currentConfig = payload.config;
      currentSecrets = payload.secrets;
      restartRequiredFields = payload.requires_restart || [];
      changedFields.clear();

      // Update path displays
      if (settingsElements.configPath) {
         settingsElements.configPath.textContent = payload.config_path || 'Unknown';
      }
      if (settingsElements.secretsPath) {
         settingsElements.secretsPath.textContent = payload.secrets_path || 'Unknown';
      }

      // Update logo from AI name (capitalize for display)
      const logoEl = document.getElementById('logo');
      if (logoEl && currentConfig.general && currentConfig.general.ai_name) {
         logoEl.textContent = currentConfig.general.ai_name.toUpperCase();
      }

      // Render settings sections
      if (callbacks.renderSettingsSections) {
         callbacks.renderSettingsSections();
      }

      // Update secrets status
      updateSecretsStatus(currentSecrets);

      // Hide restart notice initially
      if (settingsElements.restartNotice) {
         settingsElements.restartNotice.classList.add('hidden');
      }

      // Initialize audio backend state (grey out or request devices)
      const backendSelect = document.getElementById('setting-audio-backend');
      if (backendSelect && callbacks.updateAudioBackendState) {
         // Add backend change listener
         backendSelect.addEventListener('change', () => {
            callbacks.updateAudioBackendState(backendSelect.value);
         });

         // Initialize state based on current value
         callbacks.updateAudioBackendState(backendSelect.value);
      }

      // Extract cloud model lists from config for quick controls dropdown
      if (callbacks.updateCloudModelLists) {
         callbacks.updateCloudModelLists(currentConfig);
      }

      // Extract global defaults from config for new conversation reset
      if (callbacks.extractGlobalDefaults) {
         callbacks.extractGlobalDefaults(currentConfig);
      }

      // Apply defaults to conversation controls (reasoning, tools dropdowns)
      if (callbacks.applyGlobalDefaultsToControls) {
         callbacks.applyGlobalDefaultsToControls();
      }

      // Update LLM runtime controls
      if (payload.llm_runtime && callbacks.updateLlmControls) {
         callbacks.updateLlmControls(payload.llm_runtime);
      }

      // Update auth state and UI visibility
      if (callbacks.setAuthState) {
         callbacks.setAuthState({
            authenticated: payload.authenticated || false,
            isAdmin: payload.is_admin || false,
            username: payload.username || '',
         });
      }
      if (callbacks.updateAuthVisibility) {
         callbacks.updateAuthVisibility();
      }
   }

   /**
    * Update secrets status indicators in the UI
    * @param {Object} secrets - Secrets object with boolean indicators
    */
   function updateSecretsStatus(secrets) {
      if (!secrets) return;

      const updateStatus = (el, isSet) => {
         if (!el) return;
         el.textContent = isSet ? 'Set' : 'Not set';
         el.className = `secret-status ${isSet ? 'is-set' : 'not-set'}`;
      };

      updateStatus(settingsElements.statusOpenai, secrets.openai_api_key);
      updateStatus(settingsElements.statusClaude, secrets.claude_api_key);
      updateStatus(settingsElements.statusGemini, secrets.gemini_api_key);
      updateStatus(settingsElements.statusMqttUser, secrets.mqtt_username);
      updateStatus(settingsElements.statusMqttPass, secrets.mqtt_password);
   }

   /**
    * Collect all config values from the settings form
    * @returns {Object} Config object with all values
    */
   function collectConfigValues() {
      const config = {};
      const container = settingsElements.sectionsContainer;
      if (!container) return config;

      const inputs = container.querySelectorAll('[data-key]');

      inputs.forEach((input) => {
         const key = input.dataset.key;
         const parts = key.split('.');

         let value;
         if (input.type === 'checkbox') {
            value = input.checked;
         } else if (input.type === 'number') {
            value = input.value !== '' ? parseFloat(input.value) : null;
         } else if (input.dataset.type === 'model_list' || input.dataset.type === 'string_list') {
            // Convert newline-separated text to array, filtering empty lines
            value = input.value
               .split('\n')
               .map((line) => line.trim())
               .filter((line) => line.length > 0);
         } else if (input.dataset.type === 'model_default_select') {
            // Parse as integer index
            value = parseInt(input.value, 10) || 0;
         } else {
            value = input.value;
         }

         // Build nested structure
         let obj = config;
         for (let i = 0; i < parts.length - 1; i++) {
            if (!obj[parts[i]]) obj[parts[i]] = {};
            obj = obj[parts[i]];
         }
         obj[parts[parts.length - 1]] = value;
      });

      // AI name should always be lowercase (for wake word detection)
      if (config.general && config.general.ai_name) {
         config.general.ai_name = config.general.ai_name.toLowerCase();
      }

      // Map virtual tool_calling section to llm.tools.mode
      // The UI has a separate "Tool Calling" section, but backend uses llm.tools.mode
      if (config.tool_calling && config.tool_calling.mode !== undefined) {
         if (!config.llm) config.llm = {};
         if (!config.llm.tools) config.llm.tools = {};
         config.llm.tools.mode = config.tool_calling.mode;
         // Remove the virtual tool_calling section (not in backend config)
         delete config.tool_calling;
      }

      return config;
   }

   /**
    * Save config to server
    */
   function saveConfig() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Cannot save: Not connected to server', 'error');
         }
         return;
      }

      const config = collectConfigValues();

      // Show loading state (H8)
      const btn = settingsElements.saveConfigBtn;
      if (btn) {
         btn.disabled = true;
         btn.dataset.originalText = btn.textContent;
         btn.textContent = 'Saving...';
      }

      DawnWS.send({
         type: 'set_config',
         payload: config,
      });
   }

   /**
    * Save secrets to server
    */
   function saveSecrets() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Cannot save: Not connected to server', 'error');
         }
         return;
      }

      const secrets = {};

      // Only send non-empty values (don't overwrite with empty)
      if (settingsElements.secretOpenai && settingsElements.secretOpenai.value) {
         secrets.openai_api_key = settingsElements.secretOpenai.value;
      }
      if (settingsElements.secretClaude && settingsElements.secretClaude.value) {
         secrets.claude_api_key = settingsElements.secretClaude.value;
      }
      if (settingsElements.secretGemini && settingsElements.secretGemini.value) {
         secrets.gemini_api_key = settingsElements.secretGemini.value;
      }
      if (settingsElements.secretMqttUser && settingsElements.secretMqttUser.value) {
         secrets.mqtt_username = settingsElements.secretMqttUser.value;
      }
      if (settingsElements.secretMqttPass && settingsElements.secretMqttPass.value) {
         secrets.mqtt_password = settingsElements.secretMqttPass.value;
      }

      if (Object.keys(secrets).length === 0) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('No secrets entered to save', 'warning');
         }
         return;
      }

      DawnWS.send({
         type: 'set_secrets',
         payload: secrets,
      });

      // Clear inputs after sending
      if (settingsElements.secretOpenai) settingsElements.secretOpenai.value = '';
      if (settingsElements.secretClaude) settingsElements.secretClaude.value = '';
      if (settingsElements.secretGemini) settingsElements.secretGemini.value = '';
      if (settingsElements.secretMqttUser) settingsElements.secretMqttUser.value = '';
      if (settingsElements.secretMqttPass) settingsElements.secretMqttPass.value = '';
   }

   /**
    * Handle set_config response from server
    * @param {Object} payload - Response payload
    */
   function handleSetConfigResponse(payload) {
      // Restore button state (H8)
      const btn = settingsElements.saveConfigBtn;
      if (btn) {
         btn.disabled = false;
         btn.textContent = btn.dataset.originalText || 'Save Configuration';
      }

      if (payload.success) {
         // Check if any changed fields require restart
         const restartFields = getChangedRestartRequiredFields();
         if (restartFields.length > 0 && callbacks.showRestartConfirmation) {
            callbacks.showRestartConfirmation(restartFields);
         } else {
            if (typeof DawnToast !== 'undefined') {
               DawnToast.show('Configuration saved successfully!', 'success');
            }
         }

         // Clear changed fields tracking
         changedFields.clear();

         // Refresh config to update globalDefaults and other cached values
         requestConfig();
      } else {
         console.error('Failed to save config:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to save configuration: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
      }
   }

   /**
    * Handle set_secrets response from server
    * @param {Object} payload - Response payload
    */
   function handleSetSecretsResponse(payload) {
      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Secrets saved successfully!', 'success');
         }

         // Update status indicators
         if (payload.secrets) {
            updateSecretsStatus(payload.secrets);
         }

         // Refresh config to get updated secrets_path
         requestConfig();
      } else {
         console.error('Failed to save secrets:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to save secrets: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
      }
   }

   /**
    * Get list of changed fields that require restart
    * @returns {Array} Array of field keys requiring restart
    */
   function getChangedRestartRequiredFields() {
      const restartFields = [];
      for (const field of changedFields) {
         if (restartRequiredFields.includes(field)) {
            restartFields.push(field);
         }
      }
      return restartFields;
   }

   // Export for settings module
   window.DawnSettingsConfig = {
      setElements,
      setCallbacks,
      getCurrentConfig,
      getCurrentSecrets,
      getRestartRequiredFields,
      getChangedFields,
      clearChangedFields,
      markFieldChanged,
      getDynamicOptions,
      requestConfig,
      requestModelsList,
      requestInterfacesList,
      handleModelsListResponse,
      handleInterfacesListResponse,
      handleGetConfigResponse,
      updateSecretsStatus,
      collectConfigValues,
      saveConfig,
      saveSecrets,
      handleSetConfigResponse,
      handleSetSecretsResponse,
      getChangedRestartRequiredFields,
      updateDynamicSelects,
   };
})();
