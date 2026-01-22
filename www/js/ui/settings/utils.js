/**
 * DAWN Settings - Utility Functions
 * Pure utility functions for settings module
 */
(function () {
   'use strict';

   /**
    * Escape a string for use in HTML attributes
    * @param {string} str - String to escape
    * @returns {string} Escaped string
    */
   function escapeAttr(str) {
      return String(str)
         .replace(/&/g, '&amp;')
         .replace(/"/g, '&quot;')
         .replace(/'/g, '&#39;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;');
   }

   /**
    * Get a nested value from an object using dot notation
    * @param {Object} obj - The object to traverse
    * @param {string} path - Dot-separated path (e.g., 'llm.cloud.openai_models')
    * @returns {*} The value at the path, or undefined if not found
    */
   function getNestedValue(obj, path) {
      if (!obj || !path) return undefined;
      const parts = path.split('.');
      let current = obj;
      for (const part of parts) {
         if (current === null || current === undefined) return undefined;
         current = current[part];
      }
      return current;
   }

   /**
    * Format a number for display, removing floating-point precision artifacts.
    * Rounds to 6 significant digits to clean up values like 0.9200000166893005 → 0.92
    * @param {*} value - Value to format
    * @returns {string|number} Formatted number or empty string
    */
   function formatNumber(value) {
      if (value === undefined || value === null || value === '') return '';
      const num = Number(value);
      if (isNaN(num)) return '';
      // Use toPrecision to limit significant digits, then parseFloat to remove trailing zeros
      return parseFloat(num.toPrecision(6));
   }

   // Export for settings module
   window.DawnSettingsUtils = {
      escapeAttr,
      getNestedValue,
      formatNumber,
   };
})();
