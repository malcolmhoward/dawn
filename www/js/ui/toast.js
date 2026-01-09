/**
 * DAWN Toast Notifications Module
 * Simple toast notification system with auto-dismiss
 *
 * Usage:
 *   DawnToast.show('Message here')           // Default 'info' type
 *   DawnToast.show('Error!', 'error')        // Error toast
 *   DawnToast.show('Success!', 'success')    // Success toast
 */
(function(global) {
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
    container.appendChild(toast);

    // Auto-remove after 4 seconds
    setTimeout(() => {
      toast.classList.add('toast-fade-out');
      setTimeout(() => toast.remove(), 300);
    }, 4000);
  }

  // Expose globally
  global.DawnToast = {
    show: showToast
  };

})(window);
