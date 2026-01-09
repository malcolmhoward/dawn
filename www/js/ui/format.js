/**
 * DAWN Format Utilities Module
 * HTML escaping and markdown formatting with XSS protection
 *
 * Usage:
 *   DawnFormat.escapeHtml('<script>')     // Returns '&lt;script&gt;'
 *   DawnFormat.markdown('**bold**')       // Returns sanitized HTML
 */
(function(global) {
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

  // Expose globally
  global.DawnFormat = {
    escapeHtml: escapeHtml,
    markdown: formatMarkdown
  };

})(window);
