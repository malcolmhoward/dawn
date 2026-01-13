/**
 * DAWN Toast Notifications Module
 * Simple toast notification system with auto-dismiss
 *
 * Usage:
 *   DawnToast.show('Message here')           // Default 'info' type
 *   DawnToast.show('Error!', 'error')        // Error toast
 *   DawnToast.show('Success!', 'success')    // Success toast
 */
(function (global) {
   'use strict';

   /**
    * Show a toast notification
    * @param {string} message - Text to display
    * @param {string} type - Toast type: 'info', 'success', 'error', 'warning'
    */
   function showToast(message, type = 'info') {
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

      // Auto-dismiss timer
      let dismissTimer;
      const startDismissTimer = () => {
         dismissTimer = setTimeout(() => {
            toast.classList.add('toast-fade-out');
            setTimeout(() => toast.remove(), 300);
         }, 4000);
      };

      // Pause timer on hover/focus for accessibility (H10)
      toast.addEventListener('mouseenter', () => clearTimeout(dismissTimer));
      toast.addEventListener('mouseleave', startDismissTimer);
      toast.addEventListener('focus', () => clearTimeout(dismissTimer));
      toast.addEventListener('blur', startDismissTimer);

      // Make focusable for keyboard users (M11)
      toast.setAttribute('tabindex', '0');

      startDismissTimer();
   }

   // Expose globally
   global.DawnToast = {
      show: showToast,
   };
})(window);
