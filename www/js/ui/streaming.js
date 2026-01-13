/**
 * DAWN Streaming Module
 * Handles LLM streaming responses (ChatGPT-style real-time text)
 *
 * Usage:
 *   DawnStreaming.handleStart(payload)     // Handle stream_start message
 *   DawnStreaming.handleDelta(payload)     // Handle stream_delta message
 *   DawnStreaming.handleEnd(payload)       // Handle stream_end message
 *   DawnStreaming.finalize()               // Force finalize current stream
 *   DawnStreaming.setCallbacks({ onStateChange, onSaveMessage })
 */
(function(global) {
  'use strict';

  // Callbacks
  let callbacks = {
    onStateChange: null,    // (state, detail) => void - notify state changes
    onSaveMessage: null     // (role, text) => void - save message to history
  };

  // =============================================================================
  // Stream Start
  // =============================================================================

  /**
   * Handle stream_start: Create new assistant entry for streaming
   * @param {Object} payload - Stream start payload with stream_id
   */
  function handleStreamStart(payload) {
    console.log('Stream start:', payload);

    // Update status to show responding
    if (callbacks.onStateChange) {
      callbacks.onStateChange('speaking', 'Responding');
    }

    // Cancel any existing stream
    if (DawnState.streamingState.active) {
      console.warn('Stream start received while already streaming');
      finalizeStream();
    }

    // Remove placeholder if present
    const transcript = DawnElements.transcript;
    if (transcript) {
      const placeholder = transcript.querySelector('.transcript-placeholder');
      if (placeholder) {
        placeholder.remove();
      }

      // Create new streaming entry
      const entry = document.createElement('div');
      entry.className = 'transcript-entry assistant streaming';
      entry.setAttribute('aria-live', 'polite');  // Screen readers announce updates
      entry.setAttribute('aria-atomic', 'false'); // Only announce new content
      entry.innerHTML = `
        <div class="role">assistant</div>
        <div class="text"></div>
      `;
      transcript.appendChild(entry);

      // Update streaming state
      DawnState.streamingState.active = true;
      DawnState.streamingState.streamId = payload.stream_id;
      DawnState.streamingState.entryElement = entry;
      DawnState.streamingState.textElement = entry.querySelector('.text');
      DawnState.streamingState.content = '';

      transcript.scrollTop = transcript.scrollHeight;
    }

    // Start hesitation ring animation for token timing visualization
    if (typeof DawnVisualization !== 'undefined') {
      DawnVisualization.startHesitation();
    }
  }

  // =============================================================================
  // Stream Delta
  // =============================================================================

  /**
   * Handle stream_delta: Append text to current streaming entry
   * Uses debounced markdown rendering (max 10Hz) to avoid heavyweight parsing per token
   * @param {Object} payload - Stream delta payload with stream_id and delta
   */
  function handleStreamDelta(payload) {
    // Ignore deltas for different stream IDs
    if (!DawnState.streamingState.active || payload.stream_id !== DawnState.streamingState.streamId) {
      console.warn('Ignoring stale stream delta:', payload.stream_id, 'expected:', DawnState.streamingState.streamId);
      return;
    }

    // Track token timing for hesitation ring
    if (typeof DawnVisualization !== 'undefined') {
      DawnVisualization.onTokenEvent();
    }

    // Emit token event for decoupled visualization modules
    if (typeof DawnEvents !== 'undefined') {
      DawnEvents.emit('token', { timestamp: performance.now() });
    }

    // Append delta to content
    DawnState.streamingState.content += payload.delta;

    // During streaming, use plain text with line breaks (no markdown)
    // This prevents layout shifts from <p> tag margins during typing
    // Full markdown formatting is applied in finalizeStream()
    const nowMs = performance.now();
    if (nowMs - DawnState.streamingState.lastRenderMs >= DawnState.streamingState.renderDebounceMs) {
      // Strip command tags and render as plain text with line breaks
      const cleanText = DawnState.streamingState.content.replace(/<command>[\s\S]*?<\/command>/g, '');
      DawnState.streamingState.textElement.innerHTML = DawnFormat.escapeHtml(cleanText).replace(/\n/g, '<br>');
      DawnState.streamingState.lastRenderMs = nowMs;
      DawnState.streamingState.pendingRender = false;
    } else if (!DawnState.streamingState.pendingRender) {
      // Schedule a render for when debounce period ends
      DawnState.streamingState.pendingRender = true;
      const delay = DawnState.streamingState.renderDebounceMs - (nowMs - DawnState.streamingState.lastRenderMs);
      setTimeout(() => {
        if (DawnState.streamingState.active && DawnState.streamingState.pendingRender) {
          const cleanText = DawnState.streamingState.content.replace(/<command>[\s\S]*?<\/command>/g, '');
          DawnState.streamingState.textElement.innerHTML = DawnFormat.escapeHtml(cleanText).replace(/\n/g, '<br>');
          DawnState.streamingState.lastRenderMs = performance.now();
          DawnState.streamingState.pendingRender = false;
          if (DawnElements.transcript) {
            DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
          }
        }
      }, delay);
    }

    if (DawnElements.transcript) {
      DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
    }
  }

  // =============================================================================
  // Stream End
  // =============================================================================

  /**
   * Handle stream_end: Finalize streaming entry
   * @param {Object} payload - Stream end payload with stream_id
   */
  function handleStreamEnd(payload) {
    console.log('Stream end:', payload);

    // Ignore end for different stream IDs
    if (payload.stream_id !== DawnState.streamingState.streamId) {
      console.warn('Ignoring stale stream end:', payload.stream_id, 'expected:', DawnState.streamingState.streamId);
      return;
    }

    finalizeStream();
  }

  /**
   * Finalize the current streaming entry
   */
  function finalizeStream() {
    if (!DawnState.streamingState.active) {
      return;
    }

    // Update status back to idle
    if (callbacks.onStateChange) {
      callbacks.onStateChange('idle', null);
    }

    // Stop hesitation ring animation
    if (typeof DawnVisualization !== 'undefined') {
      DawnVisualization.stopHesitation();
    }

    // Final render to ensure all content is displayed (in case debounce was pending)
    if (DawnState.streamingState.textElement && DawnState.streamingState.content) {
      DawnState.streamingState.textElement.innerHTML = DawnFormat.markdown(DawnState.streamingState.content);
    }

    // Remove streaming class
    if (DawnState.streamingState.entryElement) {
      DawnState.streamingState.entryElement.classList.remove('streaming');
    }

    // Save the complete assistant message to conversation history
    if (DawnState.streamingState.content && callbacks.onSaveMessage) {
      let contentToSave = DawnState.streamingState.content;

      // Include thinking content if present (from finalized thinking block)
      if (DawnState.thinkingState.finalizedContent && DawnState.thinkingState.finalizedContent.trim()) {
        const provider = DawnState.thinkingState.finalizedProvider || 'unknown';
        const duration = DawnState.thinkingState.finalizedDuration || '0';
        // Prefix with thinking block marker
        contentToSave = `<dawn:thinking provider="${provider}" duration="${duration}">\n` +
                        DawnState.thinkingState.finalizedContent +
                        '\n</dawn:thinking>\n' + contentToSave;
      }

      // Include reasoning tokens if present (OpenAI o-series)
      if (DawnState.streamingState.reasoningTokens > 0) {
        contentToSave = `<dawn:reasoning tokens="${DawnState.streamingState.reasoningTokens}"/>\n` + contentToSave;
      }

      callbacks.onSaveMessage('assistant', contentToSave);

      // Clear finalized thinking content after saving
      DawnState.thinkingState.finalizedContent = '';
      DawnState.thinkingState.finalizedDuration = '0';
      DawnState.thinkingState.finalizedProvider = null;
    }

    // Reset state
    DawnState.streamingState.active = false;
    DawnState.streamingState.streamId = null;
    DawnState.streamingState.entryElement = null;
    DawnState.streamingState.textElement = null;
    DawnState.streamingState.content = '';
    DawnState.streamingState.pendingRender = false;
    DawnState.streamingState.reasoningTokens = 0;
  }

  /**
   * Set callbacks
   * @param {Object} cbs - Callback functions
   */
  function setCallbacks(cbs) {
    if (cbs.onStateChange) callbacks.onStateChange = cbs.onStateChange;
    if (cbs.onSaveMessage) callbacks.onSaveMessage = cbs.onSaveMessage;
  }

  // =============================================================================
  // Thinking Block Handlers (Extended Reasoning)
  // =============================================================================

  /**
   * Handle thinking_start: Create collapsible thinking block
   * @param {Object} payload - { stream_id, provider }
   */
  function handleThinkingStart(payload) {
    console.log('Thinking start:', payload);

    // Cancel any existing thinking block
    if (DawnState.thinkingState.active) {
      console.warn('Thinking start received while already thinking');
      finalizeThinking(true);
    }

    const transcript = DawnElements.transcript;
    if (!transcript) return;

    // Remove placeholder if present
    const placeholder = transcript.querySelector('.transcript-placeholder');
    if (placeholder) {
      placeholder.remove();
    }

    // Create thinking block entry
    const entry = document.createElement('div');
    entry.className = 'thinking-block collapsed';
    entry.setAttribute('role', 'region');
    entry.setAttribute('aria-label', 'AI thinking process');

    const providerLabel = payload.provider === 'claude' ? 'Claude' :
                          payload.provider === 'local' ? 'Local LLM' : 'AI';

    entry.innerHTML = `
      <div class="thinking-header" role="button" tabindex="0" aria-expanded="false">
        <span class="thinking-icon" aria-hidden="true">💭</span>
        <span class="thinking-label">${providerLabel} is thinking...</span>
        <span class="thinking-duration"></span>
        <span class="thinking-toggle" aria-hidden="true">▼</span>
      </div>
      <div class="thinking-content" aria-live="polite"></div>
    `;

    // Add click handler for toggle
    const header = entry.querySelector('.thinking-header');
    header.addEventListener('click', () => toggleThinking(entry));
    header.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        toggleThinking(entry);
      }
    });

    transcript.appendChild(entry);

    // Update thinking state
    DawnState.thinkingState.active = true;
    DawnState.thinkingState.streamId = payload.stream_id;
    DawnState.thinkingState.provider = payload.provider;
    DawnState.thinkingState.entryElement = entry;
    DawnState.thinkingState.contentElement = entry.querySelector('.thinking-content');
    DawnState.thinkingState.content = '';
    DawnState.thinkingState.collapsed = true;
    DawnState.thinkingState.startTime = performance.now();

    transcript.scrollTop = transcript.scrollHeight;
  }

  /**
   * Handle thinking_delta: Append text to thinking block
   * @param {Object} payload - { stream_id, delta }
   */
  function handleThinkingDelta(payload) {
    // Ignore deltas for inactive or mismatched streams
    if (!DawnState.thinkingState.active) {
      return;
    }

    // Append delta to content
    DawnState.thinkingState.content += payload.delta;

    // Debounced rendering (same pattern as stream_delta)
    const nowMs = performance.now();
    if (nowMs - DawnState.thinkingState.lastRenderMs >= DawnState.thinkingState.renderDebounceMs) {
      renderThinkingContent();
      DawnState.thinkingState.lastRenderMs = nowMs;
      DawnState.thinkingState.pendingRender = false;
    } else if (!DawnState.thinkingState.pendingRender) {
      DawnState.thinkingState.pendingRender = true;
      const delay = DawnState.thinkingState.renderDebounceMs - (nowMs - DawnState.thinkingState.lastRenderMs);
      setTimeout(() => {
        if (DawnState.thinkingState.active && DawnState.thinkingState.pendingRender) {
          renderThinkingContent();
          DawnState.thinkingState.lastRenderMs = performance.now();
          DawnState.thinkingState.pendingRender = false;
        }
      }, delay);
    }
  }

  /**
   * Render thinking content to the element
   */
  function renderThinkingContent() {
    if (!DawnState.thinkingState.contentElement) return;

    // Render as plain text with line breaks during streaming
    const escapedContent = DawnFormat.escapeHtml(DawnState.thinkingState.content);
    DawnState.thinkingState.contentElement.innerHTML = escapedContent.replace(/\n/g, '<br>');

    // Auto-scroll transcript
    if (DawnElements.transcript) {
      DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
    }
  }

  /**
   * Handle thinking_end: Finalize thinking block
   * @param {Object} payload - { stream_id, has_content }
   */
  function handleThinkingEnd(payload) {
    console.log('Thinking end:', payload);
    finalizeThinking(payload.has_content);
  }

  /**
   * Finalize the current thinking block
   * @param {boolean} hasContent - Whether thinking had content
   */
  function finalizeThinking(hasContent) {
    if (!DawnState.thinkingState.active) {
      return;
    }

    const entry = DawnState.thinkingState.entryElement;

    // Calculate duration for saving
    let durationSec = '0';
    if (DawnState.thinkingState.startTime) {
      const durationMs = performance.now() - DawnState.thinkingState.startTime;
      durationSec = (durationMs / 1000).toFixed(1);
    }

    // Store finalized data for saving with the assistant message
    DawnState.thinkingState.finalizedDuration = durationSec;
    DawnState.thinkingState.finalizedProvider = DawnState.thinkingState.provider || 'unknown';
    DawnState.thinkingState.finalizedContent = DawnState.thinkingState.content;

    if (entry) {
      // Update label and duration
      const label = entry.querySelector('.thinking-label');
      const duration = entry.querySelector('.thinking-duration');

      if (label) {
        const providerLabel = DawnState.thinkingState.provider === 'claude' ? 'Claude' :
                              DawnState.thinkingState.provider === 'local' ? 'Local LLM' : 'AI';
        label.textContent = hasContent ? `${providerLabel} thought` : `${providerLabel} thinking`;
      }

      if (duration) {
        duration.textContent = `(${durationSec}s)`;
      }

      // Add completed class for styling
      entry.classList.add('completed');

      // Final content render
      if (DawnState.thinkingState.contentElement && DawnState.thinkingState.content) {
        const escapedContent = DawnFormat.escapeHtml(DawnState.thinkingState.content);
        DawnState.thinkingState.contentElement.innerHTML = escapedContent.replace(/\n/g, '<br>');
      }

      // Remove if no content
      if (!hasContent && DawnState.thinkingState.content.trim() === '') {
        entry.remove();
      }
    }

    // Reset active state but preserve finalized content for saving
    DawnState.thinkingState.active = false;
    DawnState.thinkingState.streamId = null;
    DawnState.thinkingState.entryElement = null;
    DawnState.thinkingState.contentElement = null;
    DawnState.thinkingState.content = '';
    DawnState.thinkingState.pendingRender = false;
    DawnState.thinkingState.startTime = 0;
    // Note: provider, finalizedDuration, finalizedContent preserved for finalizeStream
  }

  /**
   * Toggle thinking block expanded/collapsed state
   * @param {HTMLElement} entry - The thinking block element
   */
  function toggleThinking(entry) {
    const isCollapsed = entry.classList.contains('collapsed');
    entry.classList.toggle('collapsed', !isCollapsed);

    const header = entry.querySelector('.thinking-header');

    if (header) {
      header.setAttribute('aria-expanded', isCollapsed ? 'true' : 'false');
    }
    // Note: Toggle icon rotation is handled by CSS transform, not textContent swap

    // Update state if this is the active thinking block
    if (DawnState.thinkingState.entryElement === entry) {
      DawnState.thinkingState.collapsed = !isCollapsed;
    }
  }

  // =============================================================================
  // Reasoning Summary (OpenAI o-series)
  // =============================================================================

  /**
   * Handle reasoning_summary: Create a summary block for OpenAI o-series reasoning
   * OpenAI doesn't expose reasoning content, only token count
   * @param {Object} payload - { stream_id, reasoning_tokens }
   */
  function handleReasoningSummary(payload) {
    console.log('Reasoning summary:', payload);

    const transcript = DawnElements.transcript;
    if (!transcript) return;

    const tokens = payload.reasoning_tokens || 0;
    if (tokens <= 0) return;

    // Create a collapsed thinking block that shows reasoning tokens
    const entry = document.createElement('div');
    entry.className = 'thinking-block collapsed completed reasoning-only';
    entry.setAttribute('role', 'region');
    entry.setAttribute('aria-label', 'AI reasoning summary');

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

    // Add click handler for toggle (shows the "not accessible" message)
    const header = entry.querySelector('.thinking-header');
    header.addEventListener('click', () => toggleThinking(entry));
    header.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        toggleThinking(entry);
      }
    });

    // Find the most recent assistant entry and insert before it
    const assistantEntries = transcript.querySelectorAll('.transcript-entry.assistant');
    if (assistantEntries.length > 0) {
      const lastAssistant = assistantEntries[assistantEntries.length - 1];
      transcript.insertBefore(entry, lastAssistant);
    } else {
      // Fallback: append if no assistant entry found
      transcript.appendChild(entry);
    }

    // Store reasoning tokens for saving with the message
    DawnState.streamingState.reasoningTokens = tokens;

    transcript.scrollTop = transcript.scrollHeight;
  }

  // =============================================================================
  // Export Module
  // =============================================================================

  global.DawnStreaming = {
    handleStart: handleStreamStart,
    handleDelta: handleStreamDelta,
    handleEnd: handleStreamEnd,
    finalize: finalizeStream,
    setCallbacks: setCallbacks,
    // Thinking handlers
    handleThinkingStart: handleThinkingStart,
    handleThinkingDelta: handleThinkingDelta,
    handleThinkingEnd: handleThinkingEnd,
    finalizeThinking: finalizeThinking,
    // Reasoning summary (OpenAI o-series)
    handleReasoningSummary: handleReasoningSummary
  };

})(window);
