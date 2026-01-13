/**
 * DAWN Transcript Module
 * Renders chat messages with debug/command separation
 *
 * Usage:
 *   DawnTranscript.addEntry(role, text)        // Add transcript entry (routes debug content)
 *   DawnTranscript.addDebug(label, content)    // Add debug entry directly
 *   DawnTranscript.addNormal(role, text)       // Add normal entry directly
 */
(function (global) {
   'use strict';

   // =============================================================================
   // Thinking Block Helpers
   // =============================================================================

   /**
    * Check if text contains thinking content
    * @param {string} text - Text to check
    * @returns {boolean}
    */
   function containsThinkingContent(text) {
      return text.includes('<dawn:thinking');
   }

   /**
    * Extract thinking content and remaining message from text
    * @param {string} text - Text to parse
    * @returns {{thinking: {content: string, provider: string, duration: string}|null, remaining: string}}
    */
   function extractThinkingContent(text) {
      const thinkingRegex =
         /<dawn:thinking\s+provider="([^"]+)"\s+duration="([^"]+)">\n([\s\S]*?)\n<\/dawn:thinking>\n?/;
      const match = thinkingRegex.exec(text);

      if (!match) {
         return { thinking: null, remaining: text };
      }

      return {
         thinking: {
            provider: match[1],
            duration: match[2],
            content: match[3],
         },
         remaining: text.replace(match[0], '').trim(),
      };
   }

   /**
    * Create a thinking block element for display
    * @param {Object} thinking - Thinking data {provider, duration, content}
    * @returns {HTMLElement}
    */
   function createThinkingBlock(thinking) {
      const entry = document.createElement('div');
      entry.className = 'thinking-block collapsed completed';
      entry.setAttribute('role', 'region');
      entry.setAttribute('aria-label', 'AI thinking process');

      const providerLabel =
         thinking.provider === 'claude'
            ? 'Claude'
            : thinking.provider === 'local'
              ? 'Local LLM'
              : 'AI';

      entry.innerHTML = `
      <div class="thinking-header" role="button" tabindex="0" aria-expanded="false">
        <span class="thinking-icon" aria-hidden="true">💭</span>
        <span class="thinking-label">${providerLabel} thought</span>
        <span class="thinking-duration">(${thinking.duration}s)</span>
        <span class="thinking-toggle" aria-hidden="true">▼</span>
      </div>
      <div class="thinking-content">${DawnFormat.escapeHtml(thinking.content).replace(/\n/g, '<br>')}</div>
    `;

      // Add click handler for toggle
      const header = entry.querySelector('.thinking-header');
      header.addEventListener('click', () => toggleThinkingBlock(entry));
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleThinkingBlock(entry);
         }
      });

      return entry;
   }

   /**
    * Toggle thinking block expanded/collapsed state
    * @param {HTMLElement} entry - The thinking block element
    */
   function toggleThinkingBlock(entry) {
      const isCollapsed = entry.classList.contains('collapsed');
      entry.classList.toggle('collapsed', !isCollapsed);

      const header = entry.querySelector('.thinking-header');
      if (header) {
         header.setAttribute('aria-expanded', isCollapsed ? 'true' : 'false');
      }
   }

   // =============================================================================
   // Reasoning Block Helpers (OpenAI o-series)
   // =============================================================================

   /**
    * Check if text contains reasoning token marker
    * @param {string} text - Text to check
    * @returns {boolean}
    */
   function containsReasoningContent(text) {
      return text.includes('<dawn:reasoning');
   }

   /**
    * Extract reasoning tokens and remaining message from text
    * @param {string} text - Text to parse
    * @returns {{reasoning: {tokens: number}|null, remaining: string}}
    */
   function extractReasoningContent(text) {
      const reasoningRegex = /<dawn:reasoning\s+tokens="(\d+)"\/>\n?/;
      const match = reasoningRegex.exec(text);

      if (!match) {
         return { reasoning: null, remaining: text };
      }

      return {
         reasoning: {
            tokens: parseInt(match[1], 10),
         },
         remaining: text.replace(match[0], '').trim(),
      };
   }

   /**
    * Create a reasoning block element for display (OpenAI o-series)
    * @param {Object} reasoning - Reasoning data {tokens}
    * @returns {HTMLElement}
    */
   function createReasoningBlock(reasoning) {
      const entry = document.createElement('div');
      entry.className = 'thinking-block collapsed completed reasoning-only';
      entry.setAttribute('role', 'region');
      entry.setAttribute('aria-label', 'AI reasoning summary');

      const tokens = reasoning.tokens || 0;
      entry.innerHTML = `
      <div class="thinking-header" role="button" tabindex="0" aria-expanded="false">
        <span class="thinking-icon" aria-hidden="true">🧠</span>
        <span class="thinking-label">OpenAI reasoned</span>
        <span class="thinking-duration">(${tokens.toLocaleString()} tokens)</span>
      </div>
      <div class="thinking-content">
        <em>Reasoning content is not accessible from OpenAI reasoning models.</em>
      </div>
    `;

      // Add click handler for toggle
      const header = entry.querySelector('.thinking-header');
      header.addEventListener('click', () => toggleThinkingBlock(entry));
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleThinkingBlock(entry);
         }
      });

      return entry;
   }

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
         .replace(
            /(\[Tool Result:[\s\S]*?(?:\](?=\s*$)|$))/g,
            '<span style="color:#22c55e">$1</span>'
         );
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

      // Check for thinking content in assistant messages (from history)
      if (role === 'assistant' && containsThinkingContent(text)) {
         const { thinking, remaining } = extractThinkingContent(text);
         if (thinking) {
            // Add thinking block first
            const thinkingBlock = createThinkingBlock(thinking);
            transcript.appendChild(thinkingBlock);

            // Continue processing the remaining text
            if (remaining.length > 0) {
               text = remaining;
            } else {
               transcript.scrollTop = transcript.scrollHeight;
               return;
            }
         }
      }

      // Check for reasoning content in assistant messages (from history - OpenAI o-series)
      if (role === 'assistant' && containsReasoningContent(text)) {
         const { reasoning, remaining } = extractReasoningContent(text);
         if (reasoning) {
            // Add reasoning block first (before the response)
            const reasoningBlock = createReasoningBlock(reasoning);
            transcript.appendChild(reasoningBlock);

            // Continue processing the remaining text
            if (remaining.length > 0) {
               text = remaining;
            } else {
               transcript.scrollTop = transcript.scrollHeight;
               return;
            }
         }
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
                  parsed.forEach((item) => {
                     const formatted = `[Tool Call: ${item.name}]\n${JSON.stringify(item.input, null, 2)}`;
                     addDebugEntry('tool call', formatted);
                  });
                  transcript.scrollTop = transcript.scrollHeight;
                  return;
               }
               if (firstItem.type === 'tool_result') {
                  // Format tool results nicely
                  parsed.forEach((item) => {
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
         commands.forEach((cmd) => {
            addDebugEntry('command', cmd);
         });

         // Add debug entries for tool results
         toolResults.forEach((result) => {
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
      extractUserFacingText: extractUserFacingText,
   };
})(window);
