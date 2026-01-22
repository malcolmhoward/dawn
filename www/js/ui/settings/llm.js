/**
 * DAWN Settings - LLM Controls
 * LLM runtime controls, conversation settings, and model management
 */
(function () {
   'use strict';

   // LLM runtime state (updated from server on connect)
   let llmRuntimeState = {
      type: 'local',
      provider: 'openai',
      model: '',
      openai_available: false,
      claude_available: false,
      gemini_available: false,
   };

   // Per-conversation LLM settings state
   let conversationLlmState = {
      tools_mode: 'native',
      thinking_mode: 'auto',
      reasoning_effort: 'medium',
      locked: false,
   };

   // Global defaults from config (populated on config load)
   let globalDefaults = {
      type: 'cloud',
      provider: 'openai',
      openai_model: '',
      claude_model: '',
      gemini_model: '',
      tools_mode: 'native',
      thinking_mode: 'disabled', // disabled/enabled
      reasoning_effort: 'medium', // low/medium/high - controls token budget
   };

   // Cached model lists from config and local LLM (includes default indices)
   let cloudModelLists = {
      openai: [],
      claude: [],
      gemini: [],
      openaiDefaultIdx: 0,
      claudeDefaultIdx: 0,
      geminiDefaultIdx: 0,
   };
   let localModelList = [];
   let localProviderType = 'unknown'; // 'ollama', 'llama.cpp', 'Generic', 'Unknown'

   // Local provider detection timeout
   let detectLocalProviderTimeout = null;
   const DETECT_TIMEOUT_MS = 10000; // 10 seconds

   /**
    * Get current LLM runtime state
    * @returns {Object} Runtime state object
    */
   function getLlmRuntimeState() {
      return llmRuntimeState;
   }

   /**
    * Get conversation LLM state
    * @returns {Object} Conversation LLM state
    */
   function getConversationLlmState() {
      return conversationLlmState;
   }

   /**
    * Get global defaults
    * @returns {Object} Global defaults object
    */
   function getGlobalDefaults() {
      return globalDefaults;
   }

   /**
    * Initialize LLM quick controls event listeners
    */
   function initLlmControls() {
      const typeSelect = document.getElementById('llm-type-select');
      const providerSelect = document.getElementById('llm-provider-select');
      const modelSelect = document.getElementById('llm-model-select');

      if (typeSelect) {
         typeSelect.addEventListener('change', () => {
            const newType = typeSelect.value;
            setSessionLlm({ type: newType });

            // Request local models when switching to local mode
            if (newType === 'local') {
               requestLocalModels();
            } else {
               // Switching to cloud: restore provider dropdown and update model
               if (providerSelect) {
                  // Rebuild cloud provider options
                  providerSelect.innerHTML = '';
                  providerSelect.disabled = false;
                  if (cloudModelLists.openai.length > 0) {
                     const opt = document.createElement('option');
                     opt.value = 'openai';
                     opt.textContent = 'OpenAI';
                     providerSelect.appendChild(opt);
                  }
                  if (cloudModelLists.claude.length > 0) {
                     const opt = document.createElement('option');
                     opt.value = 'claude';
                     opt.textContent = 'Claude';
                     providerSelect.appendChild(opt);
                  }
                  if (cloudModelLists.gemini.length > 0) {
                     const opt = document.createElement('option');
                     opt.value = 'gemini';
                     opt.textContent = 'Gemini';
                     providerSelect.appendChild(opt);
                  }
                  // Select default provider from global defaults
                  const defaultProvider = globalDefaults.provider || 'openai';
                  if (providerSelect.querySelector(`option[value="${defaultProvider}"]`)) {
                     providerSelect.value = defaultProvider;
                  }
                  // Also send provider to session
                  setSessionLlm({ provider: providerSelect.value });
               }
               // Populate cloud models and send model to session
               updateModelDropdownForCloud(true);
            }
         });
      }

      if (providerSelect) {
         providerSelect.addEventListener('change', () => {
            setSessionLlm({ provider: providerSelect.value });
            // Update model dropdown for new provider and send model to session
            updateModelDropdownForCloud(true);
         });
      }

      if (modelSelect) {
         modelSelect.addEventListener('change', () => {
            if (modelSelect.value) {
               setSessionLlm({ model: modelSelect.value });
            }
         });
      }
   }

   /**
    * Set the locked state for conversation LLM controls
    * @param {boolean} locked - Whether controls should be locked
    */
   function setConversationLlmLocked(locked) {
      conversationLlmState.locked = locked;
      const container = document.getElementById('llm-conversation-controls');
      const indicator = document.getElementById('llm-lock-indicator');
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      if (container) {
         container.classList.toggle('locked', locked);
      }
      if (reasoningSelect) {
         reasoningSelect.disabled = locked;
      }
      if (toolsSelect) {
         toolsSelect.disabled = locked;
      }
      if (indicator) {
         indicator.classList.toggle('hidden', !locked);
      }
   }

   /**
    * Initialize per-conversation LLM controls
    */
   function initConversationLlmControls() {
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const depthSelect = document.getElementById('reasoning-effort-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      // Helper to update depth selector enabled state based on reasoning mode
      function updateDepthEnabled() {
         if (depthSelect) {
            depthSelect.disabled = reasoningSelect && reasoningSelect.value === 'disabled';
         }
      }

      if (reasoningSelect) {
         reasoningSelect.addEventListener('change', () => {
            conversationLlmState.thinking_mode = reasoningSelect.value;
            // Update depth selector enabled state
            updateDepthEnabled();
            // Immediately update session config so thinking_mode takes effect
            if (!conversationLlmState.locked) {
               setSessionLlm({ thinking_mode: reasoningSelect.value });
            }
         });
      }

      if (depthSelect) {
         depthSelect.addEventListener('change', () => {
            conversationLlmState.reasoning_effort = depthSelect.value;
            // Immediately update session config so reasoning_effort takes effect
            if (!conversationLlmState.locked) {
               setSessionLlm({ reasoning_effort: depthSelect.value });
            }
         });
         // Initialize enabled state
         updateDepthEnabled();
      }

      if (toolsSelect) {
         toolsSelect.addEventListener('change', () => {
            conversationLlmState.tools_mode = toolsSelect.value;
            // Immediately update session config so tool_mode takes effect
            if (!conversationLlmState.locked) {
               setSessionLlm({ tool_mode: toolsSelect.value });
            }
         });
      }

      // Initial session defaults are applied after config loads via applyGlobalDefaultsToControls()

      // Tools help button popup
      const toolsHelpBtn = document.getElementById('tools-help-btn');
      const toolsHelpPopup = document.getElementById('tools-help-popup');

      if (toolsHelpBtn && toolsHelpPopup) {
         // Toggle popup on button click
         toolsHelpBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            toolsHelpPopup.classList.toggle('hidden');
         });

         // Close popup when clicking outside
         // NOTE: These document-level listeners are intentionally not removed.
         // The popup is a singleton that lives for the entire session, so cleanup
         // is not necessary and the handlers have negligible overhead when inactive.
         document.addEventListener('click', (e) => {
            if (!toolsHelpPopup.contains(e.target) && e.target !== toolsHelpBtn) {
               toolsHelpPopup.classList.add('hidden');
            }
         });

         // Close popup on Escape key
         document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && !toolsHelpPopup.classList.contains('hidden')) {
               toolsHelpPopup.classList.add('hidden');
            }
         });
      }
   }

   /**
    * Reset conversation LLM controls for a new conversation
    * Resets all controls (mode, provider, model, reasoning, tools) to global defaults
    */
   function resetConversationLlmControls() {
      setConversationLlmLocked(false);

      // Clear thinking tokens from previous conversation to prevent stale display
      if (typeof DawnState !== 'undefined') {
         DawnState.metricsState.last_thinking_tokens = 0;
      }

      const typeSelect = document.getElementById('llm-type-select');
      const providerSelect = document.getElementById('llm-provider-select');
      const modelSelect = document.getElementById('llm-model-select');
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const depthSelect = document.getElementById('reasoning-effort-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      // Reset type (local/cloud)
      if (typeSelect) {
         typeSelect.value = globalDefaults.type;
      }

      // Build session reset payload
      const sessionReset = {
         type: globalDefaults.type,
         tool_mode: globalDefaults.tools_mode,
         thinking_mode: globalDefaults.thinking_mode,
         reasoning_effort: globalDefaults.reasoning_effort,
      };

      // Reset provider and model based on type
      if (globalDefaults.type === 'cloud') {
         if (providerSelect) {
            providerSelect.value = globalDefaults.provider;
         }

         // Reset model to provider's default
         let defaultModel;
         if (globalDefaults.provider === 'claude') {
            defaultModel = globalDefaults.claude_model;
         } else if (globalDefaults.provider === 'gemini') {
            defaultModel = globalDefaults.gemini_model;
         } else {
            defaultModel = globalDefaults.openai_model;
         }

         if (modelSelect && defaultModel) {
            // Update dropdown options first
            updateModelDropdownForCloud();
            modelSelect.value = defaultModel;
         }

         sessionReset.cloud_provider = globalDefaults.provider;
         sessionReset.model = defaultModel;
      }

      // Reset reasoning dropdown to global default
      if (reasoningSelect) {
         reasoningSelect.value = globalDefaults.thinking_mode;
         conversationLlmState.thinking_mode = globalDefaults.thinking_mode;
         sessionReset.thinking_mode = globalDefaults.thinking_mode;
      }

      // Reset depth dropdown and enabled state
      if (depthSelect) {
         depthSelect.value = globalDefaults.reasoning_effort;
         conversationLlmState.reasoning_effort = globalDefaults.reasoning_effort;
         depthSelect.disabled = globalDefaults.thinking_mode === 'disabled';
      }

      // Reset tools dropdown
      if (toolsSelect) {
         toolsSelect.value = globalDefaults.tools_mode;
         conversationLlmState.tools_mode = globalDefaults.tools_mode;
      }

      // Update runtime state
      llmRuntimeState.type = globalDefaults.type;
      llmRuntimeState.provider = globalDefaults.provider;
      if (sessionReset.model) {
         llmRuntimeState.model = sessionReset.model;
      }

      // Send reset to session
      setSessionLlm(sessionReset);
   }

   /**
    * Apply LLM settings from a loaded conversation
    * @param {Object} settings - LLM settings from conversation
    * @param {boolean} isLocked - Whether the conversation is locked
    */
   function applyConversationLlmSettings(settings, isLocked) {
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const depthSelect = document.getElementById('reasoning-effort-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      if (settings) {
         if (settings.thinking_mode && reasoningSelect) {
            reasoningSelect.value = settings.thinking_mode;
            conversationLlmState.thinking_mode = settings.thinking_mode;
         }

         if (settings.reasoning_effort && depthSelect) {
            depthSelect.value = settings.reasoning_effort;
            conversationLlmState.reasoning_effort = settings.reasoning_effort;
         }
         // Update depth enabled state based on thinking mode
         if (depthSelect && reasoningSelect) {
            depthSelect.disabled = reasoningSelect.value === 'disabled';
         }

         if (settings.tools_mode && toolsSelect) {
            toolsSelect.value = settings.tools_mode;
            conversationLlmState.tools_mode = settings.tools_mode;
         }

         // Apply provider/model settings to sync UI with loaded conversation
         // The backend has already restored the LLM config - this syncs the frontend UI
         if (settings.llm_type || settings.cloud_provider || settings.model) {
            const changes = {};
            if (settings.llm_type) changes.type = settings.llm_type;
            if (settings.cloud_provider) changes.provider = settings.cloud_provider;
            if (settings.model) changes.model = settings.model;

            // Show loading state while syncing
            const llmControls = document.getElementById('llm-controls');
            if (llmControls) {
               llmControls.classList.add('llm-syncing');
            }

            // Request backend to confirm settings - response will update UI via updateLlmControls
            if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
               DawnWS.send({
                  type: 'set_session_llm',
                  payload: changes,
               });
            }
         }
      }

      setConversationLlmLocked(isLocked);
   }

   /**
    * Get current conversation LLM settings for locking
    * @returns {Object} Current LLM settings
    */
   function getConversationLlmSettings() {
      return {
         llm_type: llmRuntimeState.type,
         cloud_provider: llmRuntimeState.provider,
         model: llmRuntimeState.model,
         tools_mode: conversationLlmState.tools_mode,
         thinking_mode: conversationLlmState.thinking_mode,
         reasoning_effort: conversationLlmState.reasoning_effort,
      };
   }

   /**
    * Lock conversation LLM settings (called when first message is sent)
    * @param {string} conversationId - ID of the conversation to lock
    */
   function lockConversationLlmSettings(conversationId) {
      if (conversationLlmState.locked) {
         return; // Already locked
      }

      const settings = getConversationLlmSettings();
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'lock_conversation_llm',
            payload: {
               conversation_id: conversationId,
               llm_settings: settings,
            },
         });
      }

      // Optimistically lock the UI
      setConversationLlmLocked(true);
   }

   /**
    * Request local models from server
    */
   function requestLocalModels() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'list_llm_models' });

         // Set timeout to show "Unknown" if detection takes too long
         if (detectLocalProviderTimeout) {
            clearTimeout(detectLocalProviderTimeout);
         }
         detectLocalProviderTimeout = setTimeout(() => {
            const providerSelect = document.getElementById('llm-provider-select');
            const typeSelect = document.getElementById('llm-type-select');
            if (
               providerSelect &&
               typeSelect &&
               typeSelect.value === 'local' &&
               providerSelect.value === 'detecting'
            ) {
               providerSelect.innerHTML = '';
               const opt = document.createElement('option');
               opt.value = 'unknown';
               opt.textContent = 'Unknown';
               providerSelect.appendChild(opt);
               providerSelect.title = 'Could not detect local provider';
               console.warn('Local provider detection timed out');
            }
         }, DETECT_TIMEOUT_MS);
      }
   }

   /**
    * Handle list_llm_models response from server
    * @param {Object} payload - Response payload
    */
   function handleListLlmModelsResponse(payload) {
      // Clear detection timeout since we received a response
      if (detectLocalProviderTimeout) {
         clearTimeout(detectLocalProviderTimeout);
         detectLocalProviderTimeout = null;
      }

      localModelList = payload.models || [];
      localProviderType = payload.provider || 'Unknown';

      // Update provider dropdown to show detected local provider
      const providerSelect = document.getElementById('llm-provider-select');
      const typeSelect = document.getElementById('llm-type-select');

      if (providerSelect && typeSelect && typeSelect.value === 'local') {
         providerSelect.innerHTML = '';
         const opt = document.createElement('option');
         opt.value = localProviderType.toLowerCase();
         opt.textContent = localProviderType;
         providerSelect.appendChild(opt);
         providerSelect.disabled = true;
         providerSelect.title = `Local provider: ${localProviderType}`;
      }

      const modelSelect = document.getElementById('llm-model-select');
      if (!modelSelect) return;

      // Only update model dropdown if we're in local mode
      if (typeSelect && typeSelect.value !== 'local') return;

      modelSelect.innerHTML = '';

      // llama.cpp doesn't support model switching (only shows loaded model)
      if (localProviderType === 'llama.cpp') {
         const opt = document.createElement('option');
         opt.value = payload.current_model || '';
         opt.textContent = payload.current_model || '(server default)';
         modelSelect.appendChild(opt);
         modelSelect.disabled = true;
         modelSelect.title = 'Model switching not supported with llama.cpp';
         // Update session with the loaded model (only if changed to avoid feedback loop)
         if (payload.current_model && llmRuntimeState.model !== payload.current_model) {
            llmRuntimeState.model = payload.current_model;
            setSessionLlm({ model: payload.current_model });
         }
         return;
      }

      // Ollama or Generic - enable selection
      if (localModelList.length === 0) {
         const opt = document.createElement('option');
         opt.value = '';
         opt.textContent = 'No models available';
         modelSelect.appendChild(opt);
         modelSelect.disabled = true;
         return;
      }

      localModelList.forEach((model) => {
         const opt = document.createElement('option');
         opt.value = model.name;
         opt.textContent = model.name + (model.loaded ? ' (loaded)' : '');
         modelSelect.appendChild(opt);
      });

      // Select current model and update session (only if changed to avoid feedback loop)
      if (payload.current_model) {
         modelSelect.value = payload.current_model;
         if (llmRuntimeState.model !== payload.current_model) {
            llmRuntimeState.model = payload.current_model;
            setSessionLlm({ model: payload.current_model });
         }
      }
      modelSelect.disabled = false;
      modelSelect.title = 'Select local model';
   }

   /**
    * Update model dropdown for cloud providers
    * @param {boolean} sendToSession - Whether to send selected model to session
    */
   function updateModelDropdownForCloud(sendToSession = false) {
      const modelSelect = document.getElementById('llm-model-select');
      const providerSelect = document.getElementById('llm-provider-select');
      if (!modelSelect || !providerSelect) return;

      const provider = providerSelect.value?.toLowerCase() || 'openai';
      const models = cloudModelLists[provider] || [];

      modelSelect.innerHTML = '';

      if (models.length === 0) {
         const opt = document.createElement('option');
         opt.value = '';
         opt.textContent = 'Configure in settings';
         modelSelect.appendChild(opt);
         modelSelect.disabled = true;
         modelSelect.title = 'Add models in Settings > LLM > Cloud Settings';
         return;
      }

      models.forEach((model) => {
         const opt = document.createElement('option');
         opt.value = model;
         opt.textContent = model;
         modelSelect.appendChild(opt);
      });

      // Select current model if in list, otherwise use provider's default
      const currentModel = llmRuntimeState?.model;
      if (currentModel && models.includes(currentModel)) {
         modelSelect.value = currentModel;
      } else {
         // Use default index for this provider, fall back to first model
         let defaultIdx;
         if (provider === 'claude') {
            defaultIdx = cloudModelLists.claudeDefaultIdx;
         } else if (provider === 'gemini') {
            defaultIdx = cloudModelLists.geminiDefaultIdx;
         } else {
            defaultIdx = cloudModelLists.openaiDefaultIdx;
         }
         // Ensure index is valid
         if (defaultIdx >= 0 && defaultIdx < models.length) {
            modelSelect.value = models[defaultIdx];
         } else {
            modelSelect.value = models[0];
         }
      }

      modelSelect.disabled = false;
      modelSelect.title = 'Select cloud model';

      // Send selected model to session if requested (e.g., after provider switch)
      // Only send if model actually changed to avoid feedback loops
      if (sendToSession && modelSelect.value && llmRuntimeState.model !== modelSelect.value) {
         llmRuntimeState.model = modelSelect.value;
         setSessionLlm({ model: modelSelect.value });
      }
   }

   /**
    * Update cloud model lists from config
    * @param {Object} config - Config object
    */
   function updateCloudModelLists(config) {
      // Extract model lists and default indices from config response
      if (config?.llm?.cloud) {
         const cloud = config.llm.cloud;
         cloudModelLists.openai = cloud.openai_models || [];
         cloudModelLists.claude = cloud.claude_models || [];
         cloudModelLists.gemini = cloud.gemini_models || [];
         cloudModelLists.openaiDefaultIdx = cloud.openai_default_model_idx || 0;
         cloudModelLists.claudeDefaultIdx = cloud.claude_default_model_idx || 0;
         cloudModelLists.geminiDefaultIdx = cloud.gemini_default_model_idx || 0;
      }
   }

   /**
    * Extract global defaults from config for resetting new conversations
    * @param {Object} config - Config object
    */
   function extractGlobalDefaults(config) {
      if (!config) return;

      // LLM type (local/cloud)
      if (config.llm?.type) {
         globalDefaults.type = config.llm.type;
      }

      // Cloud provider
      if (config.llm?.cloud?.provider) {
         globalDefaults.provider = config.llm.cloud.provider;
      }

      // Default models (resolve idx to model name)
      if (config.llm?.cloud) {
         const cloud = config.llm.cloud;
         const openaiModels = cloud.openai_models || [];
         const claudeModels = cloud.claude_models || [];
         const geminiModels = cloud.gemini_models || [];
         const openaiIdx = cloud.openai_default_model_idx || 0;
         const claudeIdx = cloud.claude_default_model_idx || 0;
         const geminiIdx = cloud.gemini_default_model_idx || 0;

         globalDefaults.openai_model = openaiModels[openaiIdx] || openaiModels[0] || '';
         globalDefaults.claude_model = claudeModels[claudeIdx] || claudeModels[0] || '';
         globalDefaults.gemini_model = geminiModels[geminiIdx] || geminiModels[0] || '';
      }

      // Tools mode
      if (config.llm?.tools?.mode) {
         globalDefaults.tools_mode = config.llm.tools.mode;
      }

      // Thinking mode (Claude/local)
      if (config.llm?.thinking?.mode) {
         globalDefaults.thinking_mode = config.llm.thinking.mode;
      }

      // Reasoning effort (OpenAI o-series/GPT-5)
      if (config.llm?.thinking?.reasoning_effort) {
         globalDefaults.reasoning_effort = config.llm.thinking.reasoning_effort;
      }
   }

   /**
    * Apply global defaults to conversation LLM controls
    * Called after config loads to set initial UI state
    */
   function applyGlobalDefaultsToControls() {
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const depthSelect = document.getElementById('reasoning-effort-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      if (reasoningSelect) {
         reasoningSelect.value = globalDefaults.thinking_mode;
         conversationLlmState.thinking_mode = globalDefaults.thinking_mode;
      }

      if (depthSelect) {
         depthSelect.value = globalDefaults.reasoning_effort;
         conversationLlmState.reasoning_effort = globalDefaults.reasoning_effort;
         depthSelect.disabled = globalDefaults.thinking_mode === 'disabled';
      }

      if (toolsSelect) {
         toolsSelect.value = globalDefaults.tools_mode;
         conversationLlmState.tools_mode = globalDefaults.tools_mode;
      }

      // Send initial defaults to session
      setSessionLlm({
         tool_mode: globalDefaults.tools_mode,
         thinking_mode: globalDefaults.thinking_mode,
         reasoning_effort: globalDefaults.reasoning_effort,
      });
   }

   /**
    * Update LLM controls from runtime state
    * @param {Object} runtime - Runtime state object
    */
   function updateLlmControls(runtime) {
      llmRuntimeState = { ...llmRuntimeState, ...runtime };

      const typeSelect = document.getElementById('llm-type-select');
      const providerSelect = document.getElementById('llm-provider-select');

      if (typeSelect) {
         typeSelect.value = runtime.type || 'cloud';
      }

      if (providerSelect) {
         // Update available options based on API key availability
         providerSelect.innerHTML = '';

         if (runtime.openai_available) {
            const opt = document.createElement('option');
            opt.value = 'openai';
            opt.textContent = 'OpenAI';
            providerSelect.appendChild(opt);
         }

         if (runtime.claude_available) {
            const opt = document.createElement('option');
            opt.value = 'claude';
            opt.textContent = 'Claude';
            providerSelect.appendChild(opt);
         }

         if (runtime.gemini_available) {
            const opt = document.createElement('option');
            opt.value = 'gemini';
            opt.textContent = 'Gemini';
            providerSelect.appendChild(opt);
         }

         // If no providers available, show disabled message
         if (!runtime.openai_available && !runtime.claude_available && !runtime.gemini_available) {
            const opt = document.createElement('option');
            opt.value = '';
            opt.textContent = 'No API keys configured';
            opt.disabled = true;
            providerSelect.appendChild(opt);
            providerSelect.disabled = true;
         } else {
            providerSelect.disabled = false;
            // Set current value
            const currentProvider = runtime.provider?.toLowerCase() || 'openai';
            if (providerSelect.querySelector(`option[value="${currentProvider}"]`)) {
               providerSelect.value = currentProvider;
            }
         }
      }

      // Enable/disable provider selector based on LLM type
      if (providerSelect) {
         if (runtime.type === 'local') {
            // Show detecting state until list_llm_models_response arrives
            providerSelect.innerHTML = '';
            const opt = document.createElement('option');
            opt.value = 'detecting';
            opt.textContent = 'Detecting...';
            providerSelect.appendChild(opt);
            providerSelect.disabled = true;
            providerSelect.title = 'Auto-detecting local provider';
         } else if (
            runtime.openai_available ||
            runtime.claude_available ||
            runtime.gemini_available
         ) {
            providerSelect.disabled = false;
            providerSelect.title = 'Switch cloud provider';
         }
      }

      // Update model dropdown based on mode
      if (runtime.type === 'local') {
         // Request local models to populate dropdown
         requestLocalModels();
      } else {
         // Populate cloud models dropdown
         updateModelDropdownForCloud();
      }
   }

   /**
    * Send LLM changes to session
    * @param {Object} changes - Changes to send
    */
   function setSessionLlm(changes) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         return;
      }

      DawnWS.send({
         type: 'set_session_llm',
         payload: changes,
      });
   }

   /**
    * Handle set_session_llm response from server
    * @param {Object} payload - Response payload
    */
   function handleSetSessionLlmResponse(payload) {
      // Clear loading state
      const llmControls = document.getElementById('llm-controls');
      if (llmControls) {
         llmControls.classList.remove('llm-syncing');
      }

      if (payload.success) {
         updateLlmControls(payload);
      } else {
         console.error('Failed to update session LLM:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to switch LLM: ' + (payload.error || 'Unknown error'), 'error');
         }

         // Revert controls to actual state
         if (llmRuntimeState) {
            updateLlmControls(llmRuntimeState);
         }
      }
   }

   /**
    * Check if conversation LLM is locked
    * @returns {boolean} True if locked
    */
   function isConversationLlmLocked() {
      return conversationLlmState.locked;
   }

   // Export for settings module
   window.DawnSettingsLlm = {
      getLlmRuntimeState,
      getConversationLlmState,
      getGlobalDefaults,
      initLlmControls,
      setConversationLlmLocked,
      initConversationLlmControls,
      resetConversationLlmControls,
      applyConversationLlmSettings,
      getConversationLlmSettings,
      lockConversationLlmSettings,
      requestLocalModels,
      handleListLlmModelsResponse,
      updateModelDropdownForCloud,
      updateCloudModelLists,
      extractGlobalDefaults,
      applyGlobalDefaultsToControls,
      updateLlmControls,
      setSessionLlm,
      handleSetSessionLlmResponse,
      isConversationLlmLocked,
   };
})();
