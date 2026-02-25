/**
 * DAWN Tools Configuration Module
 * Handles LLM tool enable/disable configuration
 */
(function () {
   'use strict';

   /* =============================================================================
    * State
    * ============================================================================= */

   let toolsConfig = [];
   let toolsDirty = false;

   /* =============================================================================
    * API Communication
    * ============================================================================= */

   /**
    * Request tools configuration from server
    */
   function requestToolsConfig() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'get_tools_config' });
         console.log('Requesting tools config');
      }
   }

   /**
    * Handle get_tools_config response
    */
   function handleGetToolsConfigResponse(payload) {
      console.log('Tools config received:', payload.tools?.length || 0, 'tools');

      if (!payload.tools) {
         console.warn('No tools in response');
         return;
      }

      // Sort tools: non-armor first, then armor tools at bottom, alphabetically within each group
      toolsConfig = payload.tools.sort((a, b) => {
         const aArmor = a.armor_feature ? 1 : 0;
         const bArmor = b.armor_feature ? 1 : 0;
         if (aArmor !== bArmor) return aArmor - bArmor;
         return a.name.localeCompare(b.name);
      });
      renderToolsList();
      updateToolsTokenEstimates(payload.token_estimate);
   }

   /**
    * Handle set_tools_config response
    */
   function handleSetToolsConfigResponse(payload) {
      console.log('Tools config save:', payload.success ? 'success' : 'failed');

      if (payload.success) {
         toolsDirty = false;

         // Update token estimates with new values
         if (payload.token_estimate) {
            updateToolsTokenEstimates(payload.token_estimate);
         }

         // Notify pending saves counter if orchestrated save is in progress
         if (typeof pendingSaveCallback === 'function') {
            pendingSaveCallback(true);
            pendingSaveCallback = null;
         }
      } else {
         console.error('Failed to save tools config:', payload.error);

         if (typeof pendingSaveCallback === 'function') {
            pendingSaveCallback(false);
            pendingSaveCallback = null;
         }
      }
   }

   // Callback for orchestrated save flow
   let pendingSaveCallback = null;

   /**
    * Set callback for when tool save completes (used by config.js orchestrated save)
    */
   function onSaveComplete(cb) {
      pendingSaveCallback = cb;
   }

   /* =============================================================================
    * Rendering
    * ============================================================================= */

   /**
    * Render the tools list with checkboxes
    */
   function renderToolsList() {
      const container = document.getElementById('tools-list');
      if (!container) return;

      if (toolsConfig.length === 0) {
         container.innerHTML = '<div class="tools-loading">No tools available</div>';
         return;
      }

      toolsDirty = false;

      container.innerHTML = toolsConfig
         .map((tool) => {
            const disabledClass = !tool.available ? 'disabled' : '';
            const disabledAttr = !tool.available ? 'disabled' : '';
            // Arc reactor SVG for armor features - circle with inverted triangle
            // Equilateral triangle inscribed in circle (center 12,12, radius 10)
            const armorIcon = tool.armor_feature
               ? `<span class="armor-icon" title="OASIS armor feature">
        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.5">
          <circle cx="12" cy="12" r="10"/>
          <polygon points="12,22 3.34,7 20.66,7" stroke-linejoin="round"/>
        </svg>
      </span>`
               : '';

            // Escape tool name and description to prevent XSS
            const safeName = DawnFormat.escapeHtml(tool.name);
            const safeDesc = DawnFormat.escapeHtml(tool.description || '');

            return `
        <div class="tool-item ${disabledClass}" data-tool="${safeName}" title="${safeDesc}">
          <div class="tool-info">
            <div class="tool-name">${safeName}${armorIcon}</div>
          </div>
          <div class="tool-checkbox">
            <input type="checkbox" id="tool-local-${safeName}"
                   ${tool.local ? 'checked' : ''} ${disabledAttr}
                   data-tool="${safeName}" data-type="local">
          </div>
          <div class="tool-checkbox">
            <input type="checkbox" id="tool-remote-${safeName}"
                   ${tool.remote ? 'checked' : ''} ${disabledAttr}
                   data-tool="${safeName}" data-type="remote">
          </div>
        </div>
      `;
         })
         .join('');

      // Track tool checkbox changes for unsaved indicator
      container.addEventListener('change', (e) => {
         if (e.target.matches('input[type="checkbox"]')) {
            toolsDirty = true;
            if (typeof DawnSettings !== 'undefined' && DawnSettings.updateSaveButtonState) {
               DawnSettings.updateSaveButtonState();
            }
         }
      });
   }

   /**
    * Update the token estimate display
    */
   function updateToolsTokenEstimates(estimates) {
      if (!estimates) return;

      const localEl = document.getElementById('tools-tokens-local');
      const remoteEl = document.getElementById('tools-tokens-remote');

      if (localEl) {
         localEl.textContent = `Local: ${estimates.local || 0} tokens`;
      }
      if (remoteEl) {
         remoteEl.textContent = `Remote: ${estimates.remote || 0} tokens`;
      }
   }

   /* =============================================================================
    * Save
    * ============================================================================= */

   /**
    * Save tools configuration
    */
   function saveToolsConfig() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         return;
      }

      const tools = [];
      const container = document.getElementById('tools-list');
      const items = container.querySelectorAll('.tool-item');

      items.forEach((item) => {
         const name = item.dataset.tool;
         const localCb = item.querySelector(`input[data-type="local"]`);
         const remoteCb = item.querySelector(`input[data-type="remote"]`);

         tools.push({
            name: name,
            local: localCb ? localCb.checked : false,
            remote: remoteCb ? remoteCb.checked : false,
         });
      });

      DawnWS.send({
         type: 'set_tools_config',
         payload: { tools: tools },
      });

      console.log('Saving tools config:', tools);
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   /**
    * Re-initialize after dynamic content creation (button removed — now a no-op)
    */
   function reinitButton() {
      // Tool save button removed; saves orchestrated through main Save Configuration button
   }

   /**
    * Initialize tools section
    */
   function init() {
      // Initial button setup (may be called before dynamic creation)
      reinitButton();

      // Request tools config when settings panel opens
      const settingsBtn = document.getElementById('settings-btn');
      if (settingsBtn) {
         settingsBtn.addEventListener('click', () => {
            // Small delay to let other things initialize
            setTimeout(requestToolsConfig, 100);
         });
      }
   }

   /**
    * Get current tools config (for debugging)
    */
   function getConfig() {
      return toolsConfig;
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   window.DawnTools = {
      init: init,
      reinitButton: reinitButton,
      requestConfig: requestToolsConfig,
      handleGetConfigResponse: handleGetToolsConfigResponse,
      handleSetConfigResponse: handleSetToolsConfigResponse,
      saveToolsConfig: saveToolsConfig,
      onSaveComplete: onSaveComplete,
      hasUnsavedChanges: function () {
         return toolsDirty;
      },
      clearUnsavedChanges: function () {
         toolsDirty = false;
      },
      getConfig: getConfig,
   };
})();
