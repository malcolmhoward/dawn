/**
 * DAWN Format Utilities Module
 * HTML escaping and markdown formatting with XSS protection
 *
 * Usage:
 *   DawnFormat.escapeHtml('<script>')     // Returns '&lt;script&gt;'
 *   DawnFormat.markdown('**bold**')       // Returns sanitized HTML
 */
(function (global) {
   'use strict';

   /**
    * Escape HTML to prevent XSS
    * @param {string} str - Raw string to escape
    * @returns {string} HTML-escaped string
    */
   function escapeHtml(str) {
      if (!str) return '';
      const div = document.createElement('div');
      div.textContent = str;
      return div.innerHTML;
   }

   /**
    * Format text with full markdown support using marked.js
    * Sanitized with DOMPurify for XSS protection
    * @param {string} text - Markdown text to format
    * @returns {string} Sanitized HTML
    */
   function formatMarkdown(text) {
      // Strip command tags before rendering (they should go to debug panel only)
      const cleanText = text.replace(/<command>[\s\S]*?<\/command>/g, '');
      // Parse markdown to HTML, then sanitize
      const html = marked.parse(cleanText, { breaks: true, gfm: true });
      return DOMPurify.sanitize(html);
   }

   /**
    * Add copy buttons to all code blocks in a container
    * @param {HTMLElement} container - Container to search for code blocks
    */
   function addCopyButtons(container) {
      if (!container) return;

      container.querySelectorAll('pre').forEach((pre) => {
         // Skip if already has copy button
         if (pre.querySelector('.copy-btn')) return;

         const btn = document.createElement('button');
         btn.className = 'copy-btn';
         btn.textContent = 'Copy';
         btn.title = 'Copy to clipboard';
         btn.setAttribute('aria-label', 'Copy code to clipboard');
         btn.onclick = async (e) => {
            e.stopPropagation();
            const code = pre.querySelector('code');
            const text = code ? code.textContent : pre.textContent;
            try {
               await navigator.clipboard.writeText(text);
               btn.textContent = 'Copied!';
               setTimeout(() => (btn.textContent = 'Copy'), 2000);
            } catch (err) {
               console.error('Failed to copy:', err);
               btn.textContent = 'Failed';
               setTimeout(() => (btn.textContent = 'Copy'), 2000);
            }
         };

         // Position relative to pre block
         pre.style.position = 'relative';
         pre.appendChild(btn);
      });
   }

   /**
    * Format a date as a relative time string
    * @param {Date} date - Date to format
    * @returns {string} Relative time string (e.g., "5 mins ago", "2 days ago")
    */
   function formatRelativeTime(date) {
      const now = new Date();
      const diffMs = now - date;
      const diffMins = Math.floor(diffMs / 60000);
      const diffHours = Math.floor(diffMins / 60);
      const diffDays = Math.floor(diffHours / 24);

      if (diffMins < 1) return 'Just now';
      if (diffMins < 60) return `${diffMins} min${diffMins !== 1 ? 's' : ''} ago`;
      if (diffHours < 24) return `${diffHours} hour${diffHours !== 1 ? 's' : ''} ago`;
      if (diffDays < 7) return `${diffDays} day${diffDays !== 1 ? 's' : ''} ago`;
      return date.toLocaleDateString();
   }

   // Expose globally
   global.DawnFormat = {
      escapeHtml: escapeHtml,
      markdown: formatMarkdown,
      relativeTime: formatRelativeTime,
      addCopyButtons: addCopyButtons,
   };
})(window);
