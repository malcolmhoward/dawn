/**
 * DAWN SmartThings Integration Module
 * Handles SmartThings OAuth, device listing, and status
 *
 * Usage:
 *   DawnSmartThings.requestStatus()              // Request status from server
 *   DawnSmartThings.handleStatusResponse(p)      // Handle status response
 *   DawnSmartThings.handleAuthUrlResponse(p)     // Handle auth URL response
 *   DawnSmartThings.handleExchangeCodeResponse(p)
 *   DawnSmartThings.handleDevicesResponse(p)
 *   DawnSmartThings.handleDisconnectResponse(p)
 *   DawnSmartThings.startOAuth()                 // Start OAuth flow
 *   DawnSmartThings.refreshDevices()             // Refresh devices
 *   DawnSmartThings.disconnect()                 // Disconnect
 *   DawnSmartThings.setElements(els)             // Set UI elements
 *   DawnSmartThings.setConfirmModal(fn)          // Set confirm modal function
 */
(function (global) {
   'use strict';

   // =============================================================================
   // State
   // =============================================================================

   let state = {
      configured: false,
      authenticated: false,
      devices_count: 0,
      devices: [],
      auth_mode: 'none', // 'none', 'pat', 'oauth2'
   };

   // UI Elements (set via setElements)
   let elements = {};

   // Confirm modal function (set via setConfirmModal)
   let showConfirmModal = null;

   // Auth state callback (set via setCallbacks)
   let getAuthState = null;

   // =============================================================================
   // UI Helpers
   // =============================================================================

   /**
    * Helper to show/hide elements using CSS classes (CSP-compliant)
    */
   function stShow(el, displayClass) {
      if (!el) return;
      el.classList.remove('st-hidden', 'st-visible', 'st-visible-flex', 'st-visible-block');
      el.classList.add(displayClass || 'st-visible');
   }

   function stHide(el) {
      if (!el) return;
      el.classList.remove('st-visible', 'st-visible-flex', 'st-visible-block');
      el.classList.add('st-hidden');
   }

   // =============================================================================
   // Server Requests
   // =============================================================================

   /**
    * Request SmartThings status from server
    */
   function requestStatus() {
      if (!DawnWS.isConnected()) {
         return;
      }
      DawnWS.send({ type: 'smartthings_status' });
      console.log('Requested SmartThings status');
   }

   /**
    * Request SmartThings devices list
    */
   function requestDevices() {
      if (!DawnWS.isConnected()) {
         return;
      }
      DawnWS.send({ type: 'smartthings_list_devices' });
      console.log('Requesting SmartThings devices');
   }

   /**
    * Refresh SmartThings devices (force refresh from API)
    */
   function refreshDevices() {
      if (!DawnWS.isConnected()) {
         return;
      }
      if (elements.stRefreshBtn) {
         elements.stRefreshBtn.disabled = true;
         elements.stRefreshBtn.textContent = 'Refreshing...';
      }

      DawnWS.send({ type: 'smartthings_refresh_devices' });
      console.log('Refreshing SmartThings devices');
   }

   // =============================================================================
   // Response Handlers
   // =============================================================================

   /**
    * Handle SmartThings status response
    */
   function handleStatusResponse(payload) {
      console.log('SmartThings status:', payload);
      state.configured = payload.configured || false;
      state.authenticated = payload.authenticated || false;
      state.devices_count = payload.devices_count || 0;
      state.auth_mode = payload.auth_mode || 'none';

      updateUI();

      // If authenticated AND user is admin, also get device list
      // (device list endpoint is admin-only)
      const authState = getAuthState ? getAuthState() : {};
      if (state.authenticated && authState.isAdmin) {
         requestDevices();
      }
   }

   /**
    * Handle SmartThings auth URL response
    */
   function handleAuthUrlResponse(payload) {
      if (payload.error) {
         alert('Failed to get auth URL: ' + payload.error);
         return;
      }

      if (payload.auth_url) {
         console.log('Opening SmartThings authorization URL');
         // Open in new window for OAuth flow
         const authWindow = window.open(
            payload.auth_url,
            'smartthings_auth',
            'width=600,height=700'
         );

         // Listen for OAuth callback
         window.addEventListener('message', function handleOAuthCallback(event) {
            if (event.origin !== window.location.origin) return;
            if (event.data && event.data.type === 'smartthings_oauth_callback') {
               window.removeEventListener('message', handleOAuthCallback);
               if (authWindow) authWindow.close();

               if (event.data.code) {
                  // Exchange code for tokens (include state for CSRF protection)
                  exchangeCode(event.data.code, event.data.state);
               } else if (event.data.error) {
                  alert('OAuth failed: ' + event.data.error);
               }
            }
         });
      }
   }

   /**
    * Handle SmartThings code exchange response
    */
   function handleExchangeCodeResponse(payload) {
      if (payload.success) {
         console.log('SmartThings connected successfully');
         // Refresh status to show connected state
         requestStatus();
      } else {
         alert('Failed to connect SmartThings: ' + (payload.error || 'Unknown error'));
      }
   }

   /**
    * Handle SmartThings devices response
    */
   function handleDevicesResponse(payload) {
      // Re-enable refresh button
      if (elements.stRefreshBtn) {
         elements.stRefreshBtn.disabled = false;
         elements.stRefreshBtn.textContent = 'Refresh Devices';
      }

      if (payload.error) {
         console.error('SmartThings devices error:', payload.error);
         return;
      }

      if (payload.devices) {
         state.devices = payload.devices;
         state.devices_count = payload.devices.length;
         renderDevices(payload.devices);
         updateUI();
      }
   }

   /**
    * Handle SmartThings disconnect response
    */
   function handleDisconnectResponse(payload) {
      if (payload.success) {
         console.log('SmartThings disconnected');
         state.authenticated = false;
         state.devices = [];
         state.devices_count = 0;
         updateUI();
      } else {
         alert('Failed to disconnect: ' + (payload.error || 'Unknown error'));
      }
   }

   // =============================================================================
   // OAuth Flow
   // =============================================================================

   /**
    * Start SmartThings OAuth flow
    */
   function startOAuth() {
      if (!DawnWS.isConnected()) {
         alert('Not connected to server');
         return;
      }

      // Get auth URL from server
      const redirectUri = window.location.origin + '/smartthings/callback';
      DawnWS.send({
         type: 'smartthings_get_auth_url',
         payload: { redirect_uri: redirectUri },
      });
      console.log('Requesting SmartThings auth URL');
   }

   /**
    * Exchange OAuth code for tokens
    * @param {string} code - Authorization code from OAuth callback
    * @param {string} stateParam - State parameter for CSRF protection
    */
   function exchangeCode(code, stateParam) {
      if (!DawnWS.isConnected()) {
         return;
      }

      const redirectUri = window.location.origin + '/smartthings/callback';
      const payload = { code: code, redirect_uri: redirectUri };
      if (stateParam) {
         payload.state = stateParam;
      }
      DawnWS.send({
         type: 'smartthings_exchange_code',
         payload: payload,
      });
      console.log('Exchanging SmartThings auth code with CSRF state');
   }

   /**
    * Disconnect SmartThings
    */
   function disconnect() {
      const doDisconnect = () => {
         if (!DawnWS.isConnected()) {
            return;
         }
         DawnWS.send({ type: 'smartthings_disconnect' });
         console.log('Disconnecting SmartThings');
      };

      if (showConfirmModal) {
         showConfirmModal(
            'Disconnect SmartThings?\n\nThis will remove your saved tokens.',
            doDisconnect,
            {
               title: 'Disconnect',
               okText: 'Disconnect',
               danger: true,
            }
         );
      } else {
         if (confirm('Disconnect SmartThings?\n\nThis will remove your saved tokens.')) {
            doDisconnect();
         }
      }
   }

   // =============================================================================
   // UI Update
   // =============================================================================

   /**
    * Update SmartThings UI based on current state
    */
   function updateUI() {
      const indicator = elements.stStatusIndicator;
      const statusText = elements.stStatusText;

      if (!indicator || !statusText) return;

      // Remove all status classes
      indicator.classList.remove('not-configured', 'configured', 'connected');

      if (!state.configured) {
         // Not configured - show setup instructions
         indicator.classList.add('not-configured');
         statusText.textContent = 'Not Configured';

         stShow(elements.stNotConfigured, 'st-visible-block');
         stHide(elements.stConnectBtn);
         stHide(elements.stRefreshBtn);
         stHide(elements.stDisconnectBtn);
         stHide(elements.stDevicesCountRow);
         stHide(elements.stDevicesList);
      } else if (!state.authenticated) {
         // Configured but not authenticated
         indicator.classList.add('configured');

         if (state.auth_mode === 'oauth2') {
            // OAuth2 mode - show connect button
            statusText.textContent = 'Not Connected';
            stShow(elements.stConnectBtn);
         } else {
            // PAT mode but not working - show error
            statusText.textContent = 'Token Invalid';
            stHide(elements.stConnectBtn);
         }

         stHide(elements.stNotConfigured);
         stHide(elements.stRefreshBtn);
         stHide(elements.stDisconnectBtn);
         stHide(elements.stDevicesCountRow);
         stHide(elements.stDevicesList);
      } else {
         // Authenticated - show connected state and devices
         indicator.classList.add('connected');

         if (state.auth_mode === 'pat') {
            statusText.textContent = 'Connected (PAT)';
            // PAT is configured in secrets.toml, can't disconnect via UI
            stHide(elements.stDisconnectBtn);
         } else {
            statusText.textContent = 'Connected (OAuth2)';
            stShow(elements.stDisconnectBtn);
         }

         stHide(elements.stNotConfigured);
         stHide(elements.stConnectBtn);
         stShow(elements.stRefreshBtn);
         stShow(elements.stDevicesCountRow, 'st-visible-flex');
         if (elements.stDevicesCount) {
            elements.stDevicesCount.textContent =
               state.devices_count + ' device' + (state.devices_count !== 1 ? 's' : '');
         }

         if (state.devices.length > 0) {
            stShow(elements.stDevicesList, 'st-visible-block');
         }
      }
   }

   /**
    * Render SmartThings devices list
    */
   function renderDevices(devices) {
      const container = elements.stDevicesContainer;
      if (!container) return;

      container.innerHTML = '';

      if (devices.length === 0) {
         container.innerHTML = '<div class="st-no-devices">No devices found</div>';
         return;
      }

      devices.forEach((device) => {
         const deviceEl = document.createElement('div');
         deviceEl.className = 'st-device-item';

         // Get capability icons
         const caps = getCapabilityIcons(device.capabilities);

         deviceEl.innerHTML = `
        <div class="st-device-name">${DawnFormat.escapeHtml(device.label || device.name)}</div>
        <div class="st-device-info">
          <span class="st-device-room">${DawnFormat.escapeHtml(device.room || 'No room')}</span>
          <span class="st-device-caps">${caps}</span>
        </div>
      `;
         container.appendChild(deviceEl);
      });

      if (elements.stDevicesList) {
         elements.stDevicesList.style.display = 'block';
      }
   }

   /**
    * Get capability icons for a device
    */
   function getCapabilityIcons(capabilities) {
      const icons = [];
      const capBits = capabilities || 0;

      if (capBits & 0x0001) icons.push('&#x1F4A1;'); // Switch (lightbulb)
      if (capBits & 0x0002) icons.push('&#x1F506;'); // Dimmer (brightness)
      if (capBits & 0x0004) icons.push('&#x1F308;'); // Color
      if (capBits & 0x0010) icons.push('&#x1F321;'); // Thermostat
      if (capBits & 0x0020) icons.push('&#x1F510;'); // Lock
      if (capBits & 0x0040) icons.push('&#x1F3C3;'); // Motion
      if (capBits & 0x0080) icons.push('&#x1F6AA;'); // Contact (door)
      if (capBits & 0x0100) icons.push('&#x1F321;'); // Temperature sensor
      if (capBits & 0x0200) icons.push('&#x1F4A7;'); // Humidity
      if (capBits & 0x2000) icons.push('&#x1FA9F;'); // Window shade

      return icons.join(' ') || '&#x2699;'; // Default: gear
   }

   // =============================================================================
   // Configuration
   // =============================================================================

   /**
    * Set UI elements
    * @param {Object} els - Object with element references
    */
   function setElements(els) {
      elements = els;
   }

   /**
    * Set confirm modal function
    * @param {Function} fn - Confirm modal function
    */
   function setConfirmModalFn(fn) {
      showConfirmModal = fn;
   }

   /**
    * Get current state (for external access)
    */
   function getState() {
      return { ...state };
   }

   /**
    * Set callbacks for external dependencies
    */
   function setCallbacks(cbs) {
      if (cbs.getAuthState) getAuthState = cbs.getAuthState;
   }

   // =============================================================================
   // Export Module
   // =============================================================================

   global.DawnSmartThings = {
      requestStatus: requestStatus,
      handleStatusResponse: handleStatusResponse,
      handleAuthUrlResponse: handleAuthUrlResponse,
      handleExchangeCodeResponse: handleExchangeCodeResponse,
      handleDevicesResponse: handleDevicesResponse,
      handleDisconnectResponse: handleDisconnectResponse,
      startOAuth: startOAuth,
      refreshDevices: refreshDevices,
      disconnect: disconnect,
      setElements: setElements,
      setConfirmModal: setConfirmModalFn,
      setCallbacks: setCallbacks,
      getState: getState,
   };
})(window);
