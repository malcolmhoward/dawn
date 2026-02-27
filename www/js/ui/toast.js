/**
 * DAWN Toast Notifications Module
 * Simple toast notification system with auto-dismiss
 *
 * Usage:
 *   DawnToast.show('Message here')           // Default 'info' type
 *   DawnToast.show('Error!', 'error')        // Error toast
 *   DawnToast.show('Success!', 'success')    // Success toast
 *   DawnToast.show('Warning', 'warning', 8000, {actions: [{label: 'Fix', onClick: fn}]})
 */
(function (global) {
   'use strict';

   /**
    * Show a toast notification
    * @param {string} message - Text to display
    * @param {string} type - Toast type: 'info', 'success', 'error', 'warning'
    * @param {number} duration - Auto-dismiss in ms (default 4000, 0 = manual)
    * @param {object} options - Optional: {actions: [{label, onClick}]}
    * @returns {HTMLElement} The toast element (for attaching event listeners)
    */
   function showToast(message, type = 'info', duration = 4000, options = {}) {
      // Create toast container if it doesn't exist
      let container = document.getElementById('toast-container');
      if (!container) {
         container = document.createElement('div');
         container.id = 'toast-container';
         document.body.appendChild(container);
      }

      const toast = document.createElement('div');
      toast.className = 'toast toast-' + type;
      toast.textContent = message;

      // Append action buttons via DOM construction (no innerHTML)
      if (options.actions) {
         options.actions.forEach((action) => {
            const btn = document.createElement('button');
            btn.className = 'toast-action';
            btn.textContent = action.label;
            btn.addEventListener('click', action.onClick);
            toast.appendChild(btn);
         });
      }

      // ARIA attributes for screen reader accessibility
      // Errors use 'alert' role (assertive - interrupts user)
      // Other types use 'status' role (polite - waits for pause)
      if (type === 'error') {
         toast.setAttribute('role', 'alert');
         toast.setAttribute('aria-live', 'assertive');
      } else {
         toast.setAttribute('role', 'status');
         toast.setAttribute('aria-live', 'polite');
      }

      container.appendChild(toast);

      if (duration > 0) {
         // Auto-dismiss timer
         let dismissTimer;
         const startDismissTimer = () => {
            dismissTimer = setTimeout(() => {
               toast.classList.add('toast-fade-out');
               setTimeout(() => toast.remove(), 300);
            }, duration);
         };

         // Pause timer on hover/focus for accessibility (H10)
         toast.addEventListener('mouseenter', () => clearTimeout(dismissTimer));
         toast.addEventListener('mouseleave', startDismissTimer);
         toast.addEventListener('focus', () => clearTimeout(dismissTimer));
         toast.addEventListener('blur', startDismissTimer);

         startDismissTimer();
      }

      // Make focusable for keyboard users (M11)
      toast.setAttribute('tabindex', '0');

      return toast;
   }

   // Expose globally
   global.DawnToast = {
      show: showToast,
   };
})(window);
