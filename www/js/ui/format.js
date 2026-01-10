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
    relativeTime: formatRelativeTime
  };

})(window);
