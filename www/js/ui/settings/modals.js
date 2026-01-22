/**
 * DAWN Settings - Modal Dialogs
 * Confirm and input modal dialogs with focus trap support
 */
(function () {
   'use strict';

   // Modal state
   let confirmModalCleanup = null;
   let inputModalCleanup = null;
   let pendingConfirmCallback = null;
   let pendingInputCallback = null;
   let modalTriggerElement = null; // Track element to return focus to (M13)

   /**
    * Focus trap for modals - keeps Tab key cycling within modal
    * @param {HTMLElement} modal - Modal element to trap focus in
    * @returns {Function} Cleanup function to remove event listener
    */
   function trapFocus(modal) {
      const focusableSelectors =
         'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])';
      const focusableElements = modal.querySelectorAll(focusableSelectors);
      const firstFocusable = focusableElements[0];
      const lastFocusable = focusableElements[focusableElements.length - 1];

      function handleKeydown(e) {
         if (e.key !== 'Tab') return;

         if (e.shiftKey) {
            if (document.activeElement === firstFocusable) {
               e.preventDefault();
               lastFocusable.focus();
            }
         } else {
            if (document.activeElement === lastFocusable) {
               e.preventDefault();
               firstFocusable.focus();
            }
         }
      }

      modal.addEventListener('keydown', handleKeydown);

      // Focus first element
      if (firstFocusable) firstFocusable.focus();

      // Return cleanup function
      return () => modal.removeEventListener('keydown', handleKeydown);
   }

   /**
    * Show styled confirmation modal (replaces native confirm())
    * @param {string} message - Message to display
    * @param {Function} onConfirm - Callback if user confirms
    * @param {Object} options - Optional settings
    * @param {string} options.title - Modal title (default: "Confirm")
    * @param {string} options.okText - OK button text (default: "OK")
    * @param {string} options.cancelText - Cancel button text (default: "Cancel")
    * @param {boolean} options.danger - If true, styles OK button as danger/red
    */
   function showConfirmModal(message, onConfirm, options = {}) {
      const modal = document.getElementById('confirm-modal');
      if (!modal) {
         // Fallback to native confirm if modal not found
         if (confirm(message) && onConfirm) onConfirm();
         return;
      }

      const content = modal.querySelector('.modal-content');
      const titleEl = document.getElementById('confirm-modal-title');
      const messageEl = document.getElementById('confirm-modal-message');
      const okBtn = document.getElementById('confirm-modal-ok');
      const cancelBtn = document.getElementById('confirm-modal-cancel');

      // Set content
      if (titleEl) titleEl.textContent = options.title || 'Confirm';
      if (messageEl) messageEl.textContent = message;
      if (okBtn) okBtn.textContent = options.okText || 'OK';
      if (cancelBtn) cancelBtn.textContent = options.cancelText || 'Cancel';

      // Apply danger styling if requested
      if (content) {
         content.classList.toggle('confirm-danger', !!options.danger);
      }

      // Store callback and trigger element for focus restoration (M13)
      pendingConfirmCallback = onConfirm;
      modalTriggerElement = document.activeElement;

      // Show modal
      modal.classList.remove('hidden');
      confirmModalCleanup = trapFocus(modal);
   }

   /**
    * Hide confirmation modal
    * @param {boolean} confirmed - Whether user confirmed the action
    */
   function hideConfirmModal(confirmed) {
      const modal = document.getElementById('confirm-modal');
      if (modal) {
         modal.classList.add('hidden');
         if (confirmModalCleanup) {
            confirmModalCleanup();
            confirmModalCleanup = null;
         }
      }

      if (confirmed && pendingConfirmCallback) {
         pendingConfirmCallback();
      }
      pendingConfirmCallback = null;

      // Restore focus to trigger element (M13)
      if (modalTriggerElement && typeof modalTriggerElement.focus === 'function') {
         modalTriggerElement.focus();
         modalTriggerElement = null;
      }
   }

   /**
    * Initialize confirmation modal event handlers
    */
   function initConfirmModal() {
      const okBtn = document.getElementById('confirm-modal-ok');
      const cancelBtn = document.getElementById('confirm-modal-cancel');
      const modal = document.getElementById('confirm-modal');

      if (okBtn) {
         okBtn.addEventListener('click', () => hideConfirmModal(true));
      }
      if (cancelBtn) {
         cancelBtn.addEventListener('click', () => hideConfirmModal(false));
      }

      // Close on backdrop click
      if (modal) {
         modal.addEventListener('click', (e) => {
            if (e.target === modal) hideConfirmModal(false);
         });
      }

      // Close on Escape key
      document.addEventListener('keydown', (e) => {
         if (e.key === 'Escape' && modal && !modal.classList.contains('hidden')) {
            hideConfirmModal(false);
         }
      });
   }

   /**
    * Show styled input modal (replaces native prompt())
    * @param {string} message - Message to display
    * @param {string} defaultValue - Default value for input field
    * @param {Function} onSubmit - Callback with input value if user submits
    * @param {Object} options - Optional settings
    * @param {string} options.title - Modal title (default: "Enter Value")
    * @param {string} options.okText - OK button text (default: "OK")
    * @param {string} options.cancelText - Cancel button text (default: "Cancel")
    * @param {string} options.placeholder - Input placeholder text
    */
   function showInputModal(message, defaultValue, onSubmit, options = {}) {
      const modal = document.getElementById('input-modal');
      if (!modal) {
         // Fallback to native prompt if modal not found
         const result = prompt(message, defaultValue);
         if (result !== null && onSubmit) onSubmit(result);
         return;
      }

      const titleEl = document.getElementById('input-modal-title');
      const messageEl = document.getElementById('input-modal-message');
      const inputEl = document.getElementById('input-modal-input');
      const okBtn = document.getElementById('input-modal-ok');
      const cancelBtn = document.getElementById('input-modal-cancel');

      // Set content
      if (titleEl) titleEl.textContent = options.title || 'Enter Value';
      if (messageEl) {
         messageEl.textContent = message || '';
         messageEl.style.display = message ? 'block' : 'none';
      }
      if (inputEl) {
         inputEl.value = defaultValue || '';
         inputEl.placeholder = options.placeholder || '';
      }
      if (okBtn) okBtn.textContent = options.okText || 'OK';
      if (cancelBtn) cancelBtn.textContent = options.cancelText || 'Cancel';

      // Store callback and trigger element for focus restoration (M13)
      pendingInputCallback = onSubmit;
      modalTriggerElement = document.activeElement;

      // Show modal and focus input
      modal.classList.remove('hidden');
      inputModalCleanup = trapFocus(modal);

      // Focus and select input text
      if (inputEl) {
         inputEl.focus();
         inputEl.select();
      }
   }

   /**
    * Hide input modal
    * @param {boolean} submitted - Whether user submitted the input
    */
   function hideInputModal(submitted) {
      const modal = document.getElementById('input-modal');
      const inputEl = document.getElementById('input-modal-input');

      if (modal) {
         modal.classList.add('hidden');
         if (inputModalCleanup) {
            inputModalCleanup();
            inputModalCleanup = null;
         }
      }

      if (submitted && pendingInputCallback && inputEl) {
         pendingInputCallback(inputEl.value);
      }
      pendingInputCallback = null;

      // Restore focus to trigger element (M13)
      if (modalTriggerElement && typeof modalTriggerElement.focus === 'function') {
         modalTriggerElement.focus();
         modalTriggerElement = null;
      }
   }

   /**
    * Initialize input modal event handlers
    */
   function initInputModal() {
      const okBtn = document.getElementById('input-modal-ok');
      const cancelBtn = document.getElementById('input-modal-cancel');
      const inputEl = document.getElementById('input-modal-input');
      const modal = document.getElementById('input-modal');

      if (okBtn) {
         okBtn.addEventListener('click', () => hideInputModal(true));
      }
      if (cancelBtn) {
         cancelBtn.addEventListener('click', () => hideInputModal(false));
      }

      // Submit on Enter key in input field
      if (inputEl) {
         inputEl.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
               e.preventDefault();
               hideInputModal(true);
            }
         });
      }

      // Close on backdrop click
      if (modal) {
         modal.addEventListener('click', (e) => {
            if (e.target === modal) hideInputModal(false);
         });
      }

      // Close on Escape key
      document.addEventListener('keydown', (e) => {
         if (e.key === 'Escape' && modal && !modal.classList.contains('hidden')) {
            hideInputModal(false);
         }
      });
   }

   // Export for settings module
   window.DawnSettingsModals = {
      trapFocus,
      showConfirmModal,
      hideConfirmModal,
      initConfirmModal,
      showInputModal,
      hideInputModal,
      initInputModal,
   };
})();
