/**
 * DAWN Settings Module
 * Orchestrator for settings panel, configuration management, modals, and auth visibility
 *
 * Sub-modules:
 * - settings/utils.js - Pure utility functions
 * - settings/modals.js - Confirm and input modal dialogs
 * - settings/audio.js - Audio device management
 * - settings/config.js - Config save/load/persistence
 * - settings/llm.js - LLM runtime and conversation controls
 * - settings/schema.js - Settings schema and rendering
 */
(function () {
   'use strict';

   // Import sub-modules
   const Utils = window.DawnSettingsUtils;
   const Modals = window.DawnSettingsModals;
   const Audio = window.DawnSettingsAudio;
   const Config = window.DawnSettingsConfig;
   const Llm = window.DawnSettingsLlm;
   const Schema = window.DawnSettingsSchema;

   // Settings DOM elements (populated after init)
   const settingsElements = {};

   // Callbacks for external dependencies
   let callbacks = {
      getAuthState: null,
      setAuthState: null,
      updateHistoryButtonVisibility: null,
      restoreHistorySidebarState: null,
   };

   /* =============================================================================
    * DOM Element Initialization
    * ============================================================================= */

   function initElements() {
      settingsElements.panel = document.getElementById('settings-panel');
      settingsElements.overlay = document.getElementById('settings-overlay');
      settingsElements.closeBtn = document.getElementById('settings-close');
      settingsElements.openBtn = document.getElementById('settings-btn');
      settingsElements.configPath = document.getElementById('config-path-display');
      settingsElements.secretsPath = document.getElementById('secrets-path-display');
      settingsElements.sectionsContainer = document.getElementById('settings-sections');
      settingsElements.saveConfigBtn = document.getElementById('save-config-btn');
      settingsElements.saveSecretsBtn = document.getElementById('save-secrets-btn');
      settingsElements.resetBtn = document.getElementById('reset-config-btn');
      settingsElements.restartNotice = document.getElementById('restart-notice');

      // Secret inputs
      settingsElements.secretOpenai = document.getElementById('secret-openai');
      settingsElements.secretClaude = document.getElementById('secret-claude');
      settingsElements.secretGemini = document.getElementById('secret-gemini');
      settingsElements.secretMqttUser = document.getElementById('secret-mqtt-user');
      settingsElements.secretMqttPass = document.getElementById('secret-mqtt-pass');

      // Secret status indicators
      settingsElements.statusOpenai = document.getElementById('status-openai');
      settingsElements.statusClaude = document.getElementById('status-claude');
      settingsElements.statusGemini = document.getElementById('status-gemini');
      settingsElements.statusMqttUser = document.getElementById('status-mqtt-user');
      settingsElements.statusMqttPass = document.getElementById('status-mqtt-pass');

      // SmartThings elements
      settingsElements.stStatusIndicator = document.getElementById('st-status-indicator');
      settingsElements.stStatusText =
         settingsElements.stStatusIndicator?.querySelector('.st-status-text');
      settingsElements.stDevicesCountRow = document.getElementById('st-devices-count-row');
      settingsElements.stDevicesCount = document.getElementById('st-devices-count');
      settingsElements.stConnectBtn = document.getElementById('st-connect-btn');
      settingsElements.stRefreshBtn = document.getElementById('st-refresh-btn');
      settingsElements.stDisconnectBtn = document.getElementById('st-disconnect-btn');
      settingsElements.stDevicesList = document.getElementById('st-devices-list');
      settingsElements.stDevicesContainer = document.getElementById('st-devices-container');
      settingsElements.stNotConfigured = document.getElementById('st-not-configured');
   }

   /* =============================================================================
    * Panel Open/Close
    * ============================================================================= */

   function open() {
      if (!settingsElements.panel) return;

      settingsElements.panel.classList.remove('hidden');
      settingsElements.overlay.classList.remove('hidden');

      // Request config, models, and interfaces from server
      Config.requestConfig();
      Config.requestModelsList();
      Config.requestInterfacesList();
      if (typeof DawnSmartThings !== 'undefined') {
         DawnSmartThings.requestStatus();
      }
   }

   function close() {
      if (!settingsElements.panel) return;

      settingsElements.panel.classList.add('hidden');
      settingsElements.overlay.classList.add('hidden');
   }

   /**
    * Open settings panel and expand a specific section
    * @param {string} sectionId - The ID of the section to expand (e.g., 'my-settings-section')
    */
   function openSection(sectionId) {
      // Open the settings panel first
      open();

      // Find and expand the target section
      const targetSection = document.getElementById(sectionId);
      if (targetSection) {
         // Expand it (remove collapsed)
         targetSection.classList.remove('collapsed');

         // Scroll section into view after a brief delay for panel to open
         setTimeout(function () {
            targetSection.scrollIntoView({ behavior: 'smooth', block: 'start' });
         }, 100);

         // Trigger section-specific data loading
         triggerSectionLoad(sectionId);
      }
   }

   /**
    * Trigger data loading for sections that need it when opened programmatically
    * @param {string} sectionId - The ID of the section being opened
    */
   function triggerSectionLoad(sectionId) {
      // Allow time for WebSocket to be ready
      setTimeout(function () {
         switch (sectionId) {
            case 'my-sessions-section':
               if (typeof DawnMySessions !== 'undefined' && DawnMySessions.requestList) {
                  DawnMySessions.requestList();
               }
               break;
            case 'user-management-section':
               if (typeof DawnUserManagement !== 'undefined' && DawnUserManagement.requestList) {
                  DawnUserManagement.requestList();
               }
               break;
            // Add other sections that need data loading here
         }
      }, 150);
   }

   /* =============================================================================
    * Auth Visibility
    * ============================================================================= */

   function updateAuthVisibility() {
      const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};

      // Toggle auth classes on body - CSS handles visibility of .admin-only elements
      document.body.classList.toggle('user-is-admin', authState.isAdmin || false);
      document.body.classList.toggle('user-authenticated', authState.authenticated || false);

      // Update user badge in header
      updateUserBadge();

      // Update history button visibility
      if (callbacks.updateHistoryButtonVisibility) {
         callbacks.updateHistoryButtonVisibility();
      }

      // Restore history sidebar state on desktop (only when authenticated)
      if (callbacks.restoreHistorySidebarState) {
         callbacks.restoreHistorySidebarState();
      }
   }

   function updateUserBadge() {
      const badgeContainer = document.getElementById('user-badge-container');
      const nameEl = document.getElementById('user-badge-name');
      const roleEl = document.getElementById('user-badge-role');

      if (!badgeContainer || !nameEl || !roleEl) return;

      const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};

      if (authState.authenticated && authState.username) {
         nameEl.textContent = authState.username;
         roleEl.textContent = authState.isAdmin ? 'Admin' : 'User';
         roleEl.className = 'user-badge-role ' + (authState.isAdmin ? 'admin' : 'user');
         badgeContainer.classList.remove('hidden');
      } else {
         badgeContainer.classList.add('hidden');
      }
   }

   /* =============================================================================
    * Setting Change Handler
    * ============================================================================= */

   function handleSettingChange(key, input) {
      Config.markFieldChanged(key);

      // Check if this field requires restart
      const restartRequiredFields = Config.getRestartRequiredFields();
      if (restartRequiredFields.includes(key)) {
         if (settingsElements.restartNotice) {
            settingsElements.restartNotice.classList.remove('hidden');
         }
      }

      // If this is a model_list textarea, update any dropdowns that depend on it
      if (input.dataset.type === 'model_list') {
         Schema.updateDependentModelSelects(key, input.value);
      }
   }

   /* =============================================================================
    * Restart Confirmation
    * ============================================================================= */

   function showRestartConfirmation(changedRestartFields) {
      const fieldList = changedRestartFields.map((f) => '  • ' + f).join('\n');
      const message =
         'Configuration saved successfully!\n\n' +
         'The following changes require a restart to take effect:\n' +
         fieldList +
         '\n\n' +
         'Do you want to restart DAWN now?';

      Modals.showConfirmModal(message, requestRestart, {
         title: 'Restart Required',
         okText: 'Restart Now',
         cancelText: 'Later',
      });
   }

   function requestRestart() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Cannot restart: Not connected to server', 'error');
         }
         return;
      }

      DawnWS.send({ type: 'restart' });
   }

   function handleRestartResponse(payload) {
      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'DAWN is restarting. The page will attempt to reconnect automatically.',
               'info'
            );
         }
      } else {
         console.error('Restart failed:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to restart: ' + (payload.error || 'Unknown error'), 'error');
         }
      }
   }

   /* =============================================================================
    * System Prompt Display
    * ============================================================================= */

   function handleSystemPromptResponse(payload) {
      if (!payload.success) {
         console.warn('Failed to get system prompt:', payload.error);
         return;
      }

      // Remove any existing system prompt entry
      const existing = document.getElementById('system-prompt-entry');
      if (existing) {
         existing.remove();
      }

      // Create collapsible system prompt entry
      const entry = document.createElement('div');
      entry.id = 'system-prompt-entry';
      entry.className = 'transcript-entry debug system-prompt';

      const promptLength = payload.length || payload.prompt.length;
      const tokenEstimate = Math.round(promptLength / 4); // Rough estimate

      entry.innerHTML = `
      <div class="system-prompt-header">
        <span class="system-prompt-icon">&#x2699;</span>
        <span class="system-prompt-title">System Prompt</span>
        <span class="system-prompt-stats">${promptLength.toLocaleString()} chars (~${tokenEstimate.toLocaleString()} tokens)</span>
        <span class="system-prompt-toggle">&#x25BC;</span>
      </div>
      <div class="system-prompt-content">
        <pre>${DawnFormat.escapeHtml(payload.prompt)}</pre>
      </div>
    `;

      // Add click handler for expand/collapse
      const header = entry.querySelector('.system-prompt-header');
      header.addEventListener('click', function () {
         entry.classList.toggle('expanded');
      });

      // Insert at the top of the transcript (after placeholder if present)
      const transcript =
         typeof DawnElements !== 'undefined'
            ? DawnElements.transcript
            : document.getElementById('transcript');
      if (transcript) {
         const placeholder = transcript.querySelector('.transcript-placeholder');
         if (placeholder) {
            placeholder.after(entry);
         } else {
            transcript.prepend(entry);
         }
      }

      // Show only if debug mode is on
      if (typeof DawnState !== 'undefined' && !DawnState.getDebugMode()) {
         entry.style.display = 'none';
      }
   }

   /* =============================================================================
    * Event Listener Initialization
    * ============================================================================= */

   function initListeners() {
      // Open button
      if (settingsElements.openBtn) {
         settingsElements.openBtn.addEventListener('click', open);
      }

      // Close button
      if (settingsElements.closeBtn) {
         settingsElements.closeBtn.addEventListener('click', close);
      }

      // Overlay click to close
      if (settingsElements.overlay) {
         settingsElements.overlay.addEventListener('click', close);
      }

      // Save config button
      if (settingsElements.saveConfigBtn) {
         settingsElements.saveConfigBtn.addEventListener('click', Config.saveConfig);
      }

      // Save secrets button
      if (settingsElements.saveSecretsBtn) {
         settingsElements.saveSecretsBtn.addEventListener('click', Config.saveSecrets);
      }

      // Reset button
      if (settingsElements.resetBtn) {
         settingsElements.resetBtn.addEventListener('click', () => {
            Modals.showConfirmModal(
               'Reset all settings to defaults?\n\nThis will reload the current configuration.',
               Config.requestConfig,
               {
                  title: 'Reset Configuration',
                  okText: 'Reset',
               }
            );
         });
      }

      // Secret toggle buttons
      document.querySelectorAll('.secret-toggle').forEach((btn) => {
         btn.addEventListener('click', () => {
            const targetId = btn.dataset.target;
            if (targetId) {
               Audio.toggleSecretVisibility(targetId);
            }
         });
      });

      // Password toggle buttons (modals)
      document.querySelectorAll('.password-toggle').forEach((btn) => {
         btn.addEventListener('click', () => {
            const targetId = btn.dataset.target;
            if (targetId) {
               Audio.toggleSecretVisibility(targetId);
            }
         });
      });

      // Section header toggle (toggle on parent .settings-section)
      document.querySelectorAll('.section-header').forEach((header) => {
         // Skip user-management-section - has its own handler
         if (header.closest('#user-management-section')) {
            return;
         }
         header.addEventListener('click', () => {
            const section = header.closest('.settings-section');
            if (section) {
               section.classList.toggle('collapsed');
            }
         });
      });

      // Escape key to close (but not if a modal is open)
      document.addEventListener('keydown', (e) => {
         if (
            e.key === 'Escape' &&
            settingsElements.panel &&
            !settingsElements.panel.classList.contains('hidden')
         ) {
            // Don't close settings if a modal dialog is open
            const openModal = document.querySelector('.modal:not(.hidden)');
            if (!openModal) {
               close();
            }
         }
      });

      // LLM quick controls event listeners
      Llm.initLlmControls();

      // SmartThings initialization
      if (typeof DawnSmartThings !== 'undefined') {
         DawnSmartThings.setElements({
            stStatusIndicator: settingsElements.stStatusIndicator,
            stStatusText: settingsElements.stStatusText,
            stNotConfigured: settingsElements.stNotConfigured,
            stConnectBtn: settingsElements.stConnectBtn,
            stRefreshBtn: settingsElements.stRefreshBtn,
            stDisconnectBtn: settingsElements.stDisconnectBtn,
            stDevicesCountRow: settingsElements.stDevicesCountRow,
            stDevicesCount: settingsElements.stDevicesCount,
            stDevicesList: settingsElements.stDevicesList,
            stDevicesContainer: settingsElements.stDevicesContainer,
         });
         DawnSmartThings.setConfirmModal(Modals.showConfirmModal);
         DawnSmartThings.setCallbacks({
            getAuthState: callbacks.getAuthState,
         });

         // SmartThings button listeners
         if (settingsElements.stConnectBtn) {
            settingsElements.stConnectBtn.addEventListener('click', DawnSmartThings.startOAuth);
         }
         if (settingsElements.stRefreshBtn) {
            settingsElements.stRefreshBtn.addEventListener('click', DawnSmartThings.refreshDevices);
         }
         if (settingsElements.stDisconnectBtn) {
            settingsElements.stDisconnectBtn.addEventListener('click', DawnSmartThings.disconnect);
         }
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      initElements();

      // Wire up sub-modules with dependencies
      Config.setElements(settingsElements);
      Config.setCallbacks({
         renderSettingsSections: Schema.renderSettingsSections,
         updateAudioBackendState: Audio.updateAudioBackendState,
         updateCloudModelLists: Llm.updateCloudModelLists,
         extractGlobalDefaults: Llm.extractGlobalDefaults,
         applyGlobalDefaultsToControls: Llm.applyGlobalDefaultsToControls,
         updateLlmControls: Llm.updateLlmControls,
         setAuthState: callbacks.setAuthState,
         updateAuthVisibility: updateAuthVisibility,
         showRestartConfirmation: showRestartConfirmation,
      });

      Audio.setHandleSettingChange(handleSettingChange);

      Schema.setDependencies({
         sectionsContainer: settingsElements.sectionsContainer,
         handleSettingChange: handleSettingChange,
         getCurrentConfig: Config.getCurrentConfig,
         getRestartRequiredFields: Config.getRestartRequiredFields,
         getDynamicOptions: Config.getDynamicOptions,
      });

      // Initialize modals
      Modals.initConfirmModal();
      Modals.initInputModal();

      // Initialize listeners
      initListeners();

      // Initialize memory extraction provider/model handlers
      Config.initMemoryExtractionHandlers();
   }

   /**
    * Set callbacks for external dependencies
    */
   function setCallbacks(cbs) {
      if (cbs.getAuthState) callbacks.getAuthState = cbs.getAuthState;
      if (cbs.setAuthState) callbacks.setAuthState = cbs.setAuthState;
      if (cbs.updateHistoryButtonVisibility)
         callbacks.updateHistoryButtonVisibility = cbs.updateHistoryButtonVisibility;
      if (cbs.restoreHistorySidebarState)
         callbacks.restoreHistorySidebarState = cbs.restoreHistorySidebarState;

      // Update Config module's callbacks with setAuthState
      Config.setCallbacks({
         setAuthState: cbs.setAuthState,
         updateAuthVisibility: updateAuthVisibility,
      });
   }

   /**
    * Get SmartThings elements (for external modules)
    */
   function getSmartThingsElements() {
      return {
         stStatusIndicator: settingsElements.stStatusIndicator,
         stStatusText: settingsElements.stStatusText,
         stNotConfigured: settingsElements.stNotConfigured,
         stConnectBtn: settingsElements.stConnectBtn,
         stRefreshBtn: settingsElements.stRefreshBtn,
         stDisconnectBtn: settingsElements.stDisconnectBtn,
         stDevicesCountRow: settingsElements.stDevicesCountRow,
         stDevicesCount: settingsElements.stDevicesCount,
         stDevicesList: settingsElements.stDevicesList,
         stDevicesContainer: settingsElements.stDevicesContainer,
      };
   }

   /* =============================================================================
    * Export - Maintain backward compatibility with window.DawnSettings API
    * ============================================================================= */

   window.DawnSettings = {
      // Initialization
      init: init,
      setCallbacks: setCallbacks,

      // Panel control
      open: open,
      close: close,
      openSection: openSection,

      // Config requests (delegated to Config module)
      requestConfig: Config.requestConfig,

      // Response handlers (delegated to appropriate modules)
      handleGetConfigResponse: Config.handleGetConfigResponse,
      handleSetConfigResponse: Config.handleSetConfigResponse,
      handleSetSecretsResponse: Config.handleSetSecretsResponse,
      handleModelsListResponse: Config.handleModelsListResponse,
      handleInterfacesListResponse: Config.handleInterfacesListResponse,
      handleRestartResponse: handleRestartResponse,
      handleGetAudioDevicesResponse: Audio.handleGetAudioDevicesResponse,
      handleSystemPromptResponse: handleSystemPromptResponse,
      handleSetSessionLlmResponse: Llm.handleSetSessionLlmResponse,
      handleListLlmModelsResponse: Llm.handleListLlmModelsResponse,
      handleSetPrivateResponse: Llm.handleSetPrivateResponse,

      // LLM controls (delegated to Llm module)
      updateLlmControls: Llm.updateLlmControls,
      updateCloudModelLists: Llm.updateCloudModelLists,

      // Per-conversation LLM settings (delegated to Llm module)
      initConversationLlmControls: Llm.initConversationLlmControls,
      resetConversationLlmControls: Llm.resetConversationLlmControls,
      applyConversationLlmSettings: Llm.applyConversationLlmSettings,
      lockConversationLlmSettings: Llm.lockConversationLlmSettings,
      getConversationLlmSettings: Llm.getConversationLlmSettings,
      isConversationLlmLocked: Llm.isConversationLlmLocked,

      // Modals (delegated to Modals module)
      showConfirmModal: Modals.showConfirmModal,
      showInputModal: Modals.showInputModal,
      trapFocus: Modals.trapFocus,

      // Auth visibility
      updateAuthVisibility: updateAuthVisibility,

      // SmartThings elements
      getSmartThingsElements: getSmartThingsElements,
   };
})();
