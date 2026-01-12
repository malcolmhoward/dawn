/**
 * DAWN State Management Module
 * Centralized state with getters/setters for controlled access
 *
 * Usage:
 *   DawnState.getAppState()           // Get current app state
 *   DawnState.setAppState('speaking') // Set app state
 *   DawnState.streamingState.active   // Access complex state objects
 *   DawnState.authState.isAdmin       // Check auth state
 */
(function(global) {
  'use strict';

  // =============================================================================
  // Simple State Variables
  // =============================================================================
  let currentState = 'idle';
  let debugMode = false;
  let isRecording = false;
  let audioSupported = false;

  // =============================================================================
  // Complex State Objects
  // =============================================================================

  /**
   * LLM streaming state (ChatGPT-style real-time text)
   */
  const streamingState = {
    active: false,
    streamId: null,
    entryElement: null,
    textElement: null,
    content: '',
    lastRenderMs: 0,
    renderDebounceMs: 100,
    pendingRender: false,
    reasoningTokens: 0  // OpenAI o-series reasoning token count
  };

  /**
   * Real-time metrics state (for multi-ring visualization)
   */
  const metricsState = {
    state: 'idle',
    ttft_ms: 0,
    token_rate: 0,
    context_percent: 0,
    lastUpdate: 0,
    last_ttft_ms: 0,
    last_token_rate: 0
  };

  /**
   * Hesitation ring state (token timing variance)
   */
  const hesitationState = {
    dtWindow: [],
    windowSize: 16,
    tPrevMs: 0,
    loadSmooth: 0,
    lastTokenMs: 0,
    animationId: null,
    runningSum: 0,
    runningSumSq: 0
  };

  /**
   * Extended thinking state (Claude/llama.cpp reasoning display)
   */
  const thinkingState = {
    active: false,
    streamId: null,
    provider: null,
    entryElement: null,
    contentElement: null,
    content: '',
    collapsed: true,
    startTime: 0,
    lastRenderMs: 0,
    renderDebounceMs: 100,
    pendingRender: false,
    // Finalized values for saving with the assistant message
    finalizedContent: '',
    finalizedDuration: '0',
    finalizedProvider: null
  };

  /**
   * Auth state (populated from session response)
   */
  const authState = {
    authenticated: false,
    isAdmin: false,
    username: ''
  };

  // =============================================================================
  // Expose globally
  // =============================================================================
  global.DawnState = {
    // Simple state getters/setters
    getAppState: () => currentState,
    setAppState: (state) => { currentState = state; },

    getDebugMode: () => debugMode,
    setDebugMode: (mode) => { debugMode = mode; },

    getIsRecording: () => isRecording,
    setIsRecording: (recording) => { isRecording = recording; },

    getAudioSupported: () => audioSupported,
    setAudioSupported: (supported) => { audioSupported = supported; },

    // Complex state objects (direct access)
    streamingState: streamingState,
    metricsState: metricsState,
    hesitationState: hesitationState,
    thinkingState: thinkingState,
    authState: authState
  };

})(window);
