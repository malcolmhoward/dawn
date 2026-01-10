/**
 * DAWN Transcript Module
 * Renders chat messages with debug/command separation
 *
 * Usage:
 *   DawnTranscript.addEntry(role, text)        // Add transcript entry (routes debug content)
 *   DawnTranscript.addDebug(label, content)    // Add debug entry directly
 *   DawnTranscript.addNormal(role, text)       // Add normal entry directly
 */
(function(global) {
  'use strict';

  // =============================================================================
  // Content Detection Helpers
  // =============================================================================

  /**
   * Check if text contains command tags
   * @param {string} text - Text to check
   * @returns {boolean}
   */
  function containsCommandTags(text) {
    return text.includes('<command>') || text.includes('[Tool Result:');
  }

  /**
   * Check if text is ONLY debug content (no user-facing text)
   * @param {string} text - Text to check
   * @returns {boolean}
   */
  function isOnlyDebugContent(text) {
    // Remove all command tags and tool results, see if anything meaningful remains
    const stripped = text
      .replace(/<command>[\s\S]*?<\/command>/g, '')
      .replace(/\[Tool Result:[\s\S]*?\]/g, '')
      .trim();
    return stripped.length === 0;
  }

  /**
   * Extract non-command text from a mixed message
   * @param {string} text - Text to extract from
   * @returns {string}
   */
  function extractUserFacingText(text) {
    return text
      .replace(/<command>[\s\S]*?<\/command>/g, '')
      .replace(/\[Tool Result:[\s\S]*?\]/g, '')
      .trim();
  }

  /**
   * Extract command/debug portions from a message
   * @param {string} text - Text to extract from
   * @returns {{commands: string[], toolResults: string[]}}
   */
  function extractDebugContent(text) {
    const commands = [];
    const toolResults = [];

    // Extract command tags
    const cmdRegex = /<command>([\s\S]*?)<\/command>/g;
    let match;
    while ((match = cmdRegex.exec(text)) !== null) {
      commands.push(match[0]);
    }

    // Extract tool results
    const toolRegex = /\[Tool Result:[\s\S]*?\]/g;
    while ((match = toolRegex.exec(text)) !== null) {
      toolResults.push(match[0]);
    }

    return { commands, toolResults };
  }

  // =============================================================================
  // Entry Formatting
  // =============================================================================

  /**
   * Format command text for debug display
   * @param {string} text - Text to format
   * @returns {string}
   */
  function formatCommandText(text) {
    // Highlight <command> tags in cyan, tool results in green
    return text
      .replace(/(<command>)/g, '<span style="color:#22d3ee">$1</span>')
      .replace(/(<\/command>)/g, '<span style="color:#22d3ee">$1</span>')
      .replace(/(\[Tool Result:[^\]]*\])/g, '<span style="color:#22c55e">$1</span>');
  }

  // =============================================================================
  // Entry Rendering
  // =============================================================================

  /**
   * Add a debug entry to the transcript
   * @param {string} label - Debug label (command, tool result, etc.)
   * @param {string} content - Debug content
   */
  function addDebugEntry(label, content) {
    const transcript = DawnElements.transcript;
    if (!transcript) return;

    const entry = document.createElement('div');

    // Determine specific debug class based on label
    let debugClass = 'debug';
    if (label === 'command') {
      debugClass = 'debug command';
    } else if (label === 'tool result') {
      debugClass = 'debug tool-result';
    } else if (label === 'tool call') {
      debugClass = 'debug tool-call';
    }

    entry.className = `transcript-entry ${debugClass}`;
    entry.innerHTML = `
      <div class="role">${DawnFormat.escapeHtml(label)}</div>
      <div class="text">${formatCommandText(DawnFormat.escapeHtml(content))}</div>
    `;
    // Visibility controlled by CSS via body.debug-mode class
    transcript.appendChild(entry);
  }

  /**
   * Add a normal entry to the transcript
   * @param {string} role - Message role (user, assistant, system)
   * @param {string} text - Message text
   */
  function addNormalEntry(role, text) {
    const transcript = DawnElements.transcript;
    if (!transcript) return;

    const entry = document.createElement('div');
    entry.className = `transcript-entry ${role}`;
    entry.innerHTML = `
      <div class="role">${DawnFormat.escapeHtml(role)}</div>
      <div class="text">${DawnFormat.markdown(text)}</div>
    `;
    transcript.appendChild(entry);
  }

  /**
   * Add a transcript entry with automatic debug content routing
   * @param {string} role - Message role
   * @param {string} text - Message text
   */
  function addTranscriptEntry(role, text) {
    const transcript = DawnElements.transcript;
    if (!transcript) return;

    // Remove placeholder if present
    const placeholder = transcript.querySelector('.transcript-placeholder');
    if (placeholder) {
      placeholder.remove();
    }

    // Route tool role messages to debug entries (server sends role:"tool" for tool results)
    if (role === 'tool') {
      addDebugEntry('tool result', text);
      transcript.scrollTop = transcript.scrollHeight;
      return;
    }

    // Detect Claude native tool format: JSON arrays with tool_use or tool_result objects
    // These come from conversation history and look like:
    // [ { "type": "tool_use", "id": "...", "name": "...", "input": {...} } ]
    // [ { "type": "tool_result", "tool_use_id": "...", "content": "..." } ]
    if (text.trimStart().startsWith('[')) {
      try {
        const parsed = JSON.parse(text);
        if (Array.isArray(parsed) && parsed.length > 0) {
          const firstItem = parsed[0];
          if (firstItem.type === 'tool_use') {
            // Format tool calls nicely
            parsed.forEach(item => {
              const formatted = `[Tool Call: ${item.name}]\n${JSON.stringify(item.input, null, 2)}`;
              addDebugEntry('tool call', formatted);
            });
            transcript.scrollTop = transcript.scrollHeight;
            return;
          }
          if (firstItem.type === 'tool_result') {
            // Format tool results nicely
            parsed.forEach(item => {
              const formatted = `[Tool Result: ${item.tool_use_id}]\n${item.content}`;
              addDebugEntry('tool result', formatted);
            });
            transcript.scrollTop = transcript.scrollHeight;
            return;
          }
        }
      } catch (e) {
        // Not valid JSON, continue with normal processing
      }
    }

    // Special case: Tool calls/results are sent as complete messages
    // These can contain ] characters in the content, so don't try to parse with regex
    if (text.startsWith('[Tool Result:')) {
      addDebugEntry('tool result', text);
      transcript.scrollTop = transcript.scrollHeight;
      return;
    }
    if (text.startsWith('[Tool Call:')) {
      addDebugEntry('tool call', text);
      transcript.scrollTop = transcript.scrollHeight;
      return;
    }

    const hasDebugContent = containsCommandTags(text);

    if (!hasDebugContent) {
      // Pure user-facing message - show normally
      addNormalEntry(role, text);
    } else if (isOnlyDebugContent(text)) {
      // Pure debug message (only commands/tool results) - debug only
      addDebugEntry(`debug (${role})`, text);
    } else {
      // Mixed message - show user-facing text normally AND debug content separately
      const userText = extractUserFacingText(text);
      const { commands, toolResults } = extractDebugContent(text);

      // Add debug entries for commands
      commands.forEach(cmd => {
        addDebugEntry('command', cmd);
      });

      // Add debug entries for tool results
      toolResults.forEach(result => {
        addDebugEntry('tool result', result);
      });

      // Add user-facing text if any
      if (userText.length > 0) {
        addNormalEntry(role, userText);
      }
    }

    transcript.scrollTop = transcript.scrollHeight;
  }

  // =============================================================================
  // Export Module
  // =============================================================================

  global.DawnTranscript = {
    addEntry: addTranscriptEntry,
    addDebug: addDebugEntry,
    addNormal: addNormalEntry,
    // Expose helpers for other modules
    containsCommandTags: containsCommandTags,
    isOnlyDebugContent: isOnlyDebugContent,
    extractUserFacingText: extractUserFacingText
  };

})(window);
