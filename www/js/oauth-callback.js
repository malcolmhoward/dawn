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
      /* Detect origin mismatch: if the opener is on a different origin
       * (e.g. IP vs FQDN, different port), postMessage will silently fail.
       * Try to read opener.location.origin — if cross-origin, this throws. */
      let originMatch = true;
      try {
         /* Same-origin: readable. Cross-origin: throws SecurityError. */
         void window.opener.location.origin;
      } catch (e) {
         originMatch = false;
      }

      if (!originMatch) {
         const msg = document.getElementById('msg');
         if (msg) {
            msg.innerHTML =
               '<strong>Origin mismatch</strong><br><br>' +
               'The OAuth redirect landed on <code>' +
               window.location.origin +
               '</code>, but the WebUI was opened from a different origin.<br><br>' +
               'Make sure you access the WebUI using the same URL as the ' +
               '<code>redirect_url</code> in <code>secrets.toml</code>.' +
               (code
                  ? '<br><br>Authorization succeeded, but the code could not be ' +
                    'forwarded. Please try again from the correct URL.'
                  : '');
         }
         return;
      }

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
