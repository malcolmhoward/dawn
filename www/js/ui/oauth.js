/**
 * DAWN OAuth Module
 * Manages the OAuth popup flow for Google (and future providers).
 * Opens popup synchronously on click to avoid popup blockers,
 * then navigates when the server responds with the auth URL.
 */
(function () {
   'use strict';

   let popup = null;
   let messageHandler = null;
   let pollInterval = null;
   let resolvePromise = null;
   let rejectPromise = null;

   function cleanup() {
      if (messageHandler) {
         window.removeEventListener('message', messageHandler);
         messageHandler = null;
      }
      if (pollInterval) {
         clearInterval(pollInterval);
         pollInterval = null;
      }
      if (popup && !popup.closed) {
         popup.close();
      }
      popup = null;
      resolvePromise = null;
      rejectPromise = null;
   }

   /**
    * Start the OAuth flow. Must be called from a click handler.
    * Opens the popup immediately (before async), then requests
    * the auth URL via WebSocket.
    *
    * @param {string} provider - e.g. "google"
    * @param {string} scopes - space-separated scopes
    * @returns {Promise} resolves with {code, state} or rejects on error/timeout
    */
   function startFlow(provider, scopes) {
      /* Open popup immediately in click handler to avoid popup blocker */
      popup = window.open('about:blank', 'dawn_oauth', 'width=500,height=700');

      return new Promise(function (resolve, reject) {
         resolvePromise = resolve;
         rejectPromise = reject;

         /* Timeout after 5 minutes */
         var timeout = setTimeout(function () {
            cleanup();
            reject(new Error('Authorization timed out'));
         }, 300000);

         /* Listen for postMessage from the callback page */
         messageHandler = function (event) {
            if (event.origin !== window.location.origin) return;
            if (!event.data || event.data.type !== 'oauth_callback') return;

            clearTimeout(timeout);

            if (event.data.error) {
               cleanup();
               reject(new Error(event.data.error));
               return;
            }

            /* Exchange code via WebSocket */
            if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
               DawnWS.send({
                  type: 'oauth_exchange_code',
                  payload: { code: event.data.code, state: event.data.state },
               });
            }

            cleanup();
            resolve({ code: event.data.code, state: event.data.state });
         };
         window.addEventListener('message', messageHandler);

         /* Poll for popup closed by user */
         pollInterval = setInterval(function () {
            if (popup && popup.closed) {
               clearTimeout(timeout);
               cleanup();
               reject(new Error('Authorization window was closed'));
            }
         }, 500);

         /* Request auth URL from server */
         if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
            DawnWS.send({
               type: 'oauth_get_auth_url',
               payload: { provider: provider, scopes: scopes || '' },
            });
         } else {
            clearTimeout(timeout);
            cleanup();
            reject(new Error('WebSocket not connected'));
         }
      });
   }

   /**
    * Handle the auth URL response from the server.
    * Navigates the already-open popup to Google's consent screen.
    */
   function handleAuthUrlResponse(payload) {
      if (!payload.success) {
         if (rejectPromise) {
            rejectPromise(new Error(payload.error || 'Failed to get auth URL'));
         }
         cleanup();
         return;
      }
      if (popup && !popup.closed) {
         popup.location.href = payload.url;
      } else {
         /* Popup was blocked — provide manual link */
         if (rejectPromise) {
            rejectPromise(new Error('popup_blocked'));
         }
         cleanup();
      }
   }

   /**
    * Handle the code exchange response from the server.
    */
   function handleExchangeCodeResponse(payload) {
      /* This is handled in calendar-accounts.js after the promise resolves */
      /* Just dispatch a custom event so the UI can react */
      window.dispatchEvent(
         new CustomEvent('dawn-oauth-exchange', {
            detail: payload,
         })
      );
   }

   /**
    * Disconnect an OAuth account (revoke + delete tokens).
    */
   function disconnect(provider, accountKey) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'oauth_disconnect',
            payload: { provider: provider, account_key: accountKey },
         });
      }
   }

   window.DawnOAuth = {
      startFlow: startFlow,
      handleAuthUrlResponse: handleAuthUrlResponse,
      handleExchangeCodeResponse: handleExchangeCodeResponse,
      disconnect: disconnect,
   };
})();
