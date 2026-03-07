(function () {
   'use strict';

   const form = document.getElementById('login-form');
   const errorEl = document.getElementById('error-message');
   const loginBtn = document.getElementById('login-btn');
   const versionEl = document.getElementById('version-info');

   let csrfToken = null;

   // Fetch version info
   fetch('/health')
      .then((r) => r.json())
      .then((data) => {
         versionEl.textContent = `v${data.version} (${data.git_sha.slice(0, 7)})`;
      })
      .catch(() => {
         versionEl.textContent = 'DAWN';
      });

   // Fetch CSRF token
   async function fetchCsrfToken() {
      try {
         const response = await fetch('/api/auth/csrf', { credentials: 'same-origin' });
         const data = await response.json();
         csrfToken = data.csrf_token;
      } catch (err) {
         console.error('Failed to fetch CSRF token:', err);
      }
   }

   // Fetch token on page load
   fetchCsrfToken();

   function showError(message) {
      errorEl.textContent = message;
      errorEl.classList.add('visible');
   }

   function hideError() {
      errorEl.classList.remove('visible');
   }

   function setLoading(loading) {
      loginBtn.disabled = loading;
      loginBtn.classList.toggle('loading', loading);
   }

   form.addEventListener('submit', async function (e) {
      e.preventDefault();
      hideError();

      const username = document.getElementById('username').value.trim();
      const password = document.getElementById('password').value;

      if (!username || !password) {
         showError('Please enter both username and password');
         return;
      }

      if (!csrfToken) {
         showError('Security token not loaded. Please refresh the page.');
         return;
      }

      setLoading(true);

      try {
         const rememberMe = document.getElementById('remember-me').checked;
         const response = await fetch('/api/auth/login', {
            method: 'POST',
            headers: {
               'Content-Type': 'application/json',
            },
            body: JSON.stringify({
               username,
               password,
               csrf_token: csrfToken,
               remember_me: rememberMe,
            }),
            credentials: 'same-origin',
         });

         const data = await response.json();

         if (response.ok && data.success) {
            // Redirect to main app
            window.location.href = '/';
         } else {
            showError(data.error || 'Invalid username or password');
            // Refresh CSRF token after failed attempt (token may have expired)
            fetchCsrfToken();
         }
      } catch (err) {
         console.error('Login error:', err);
         showError('Connection error. Please try again.');
      } finally {
         setLoading(false);
      }
   });

   // Check if already logged in
   fetch('/api/auth/status', { credentials: 'same-origin' })
      .then((r) => r.json())
      .then((data) => {
         if (data.authenticated) {
            window.location.href = '/';
         }
      })
      .catch(() => {});
})();
