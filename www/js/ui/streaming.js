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
      callbacks.onSaveMessage('assistant', DawnState.streamingState.content);
    }

    // Reset state
    DawnState.streamingState.active = false;
    DawnState.streamingState.streamId = null;
    DawnState.streamingState.entryElement = null;
    DawnState.streamingState.textElement = null;
    DawnState.streamingState.content = '';
    DawnState.streamingState.pendingRender = false;
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
  // Export Module
  // =============================================================================

  global.DawnStreaming = {
    handleStart: handleStreamStart,
    handleDelta: handleStreamDelta,
    handleEnd: handleStreamEnd,
    finalize: finalizeStream,
    setCallbacks: setCallbacks
  };

})(window);
