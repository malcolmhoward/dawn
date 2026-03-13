/**
 * OAuth callback handler — served from /oauth/callback.
 * Receives the authorization code from Google and forwards it
 * to the opener window via postMessage, then closes.
 */
(function () {
   'use strict';

   const params = new URLSearchParams(window.location.search);
   const code = params.get('code');
   const state = params.get('state');
   const error = params.get('error');

   if (window.opener) {
      window.opener.postMessage(
         { type: 'oauth_callback', code: code, state: state, error: error },
         window.location.origin
      );
      setTimeout(function () {
         window.close();
      }, 500);
   } else {
      document.body.textContent =
         'Authorization ' + (code ? 'successful' : 'failed') + '. You can close this window.';
   }
})();
