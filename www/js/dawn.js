/**
 * DAWN WebUI JavaScript
 * Phase 4: Voice input and audio playback
 */

(function() {
  'use strict';

  // =============================================================================
  // Configuration
  // =============================================================================
  const WS_SUBPROTOCOL = 'dawn-1.0';
  const RECONNECT_BASE_DELAY = 1000;
  const RECONNECT_MAX_DELAY = 30000;

  // Audio configuration
  // 48kHz is Opus native rate - server resamples to 16kHz for ASR
  const AUDIO_SAMPLE_RATE = 48000;
  const AUDIO_CHANNELS = 1;         // Mono
  let audioChunkMs = 200;           // Send audio every N ms (updated from server config)

  // Binary message types (match server)
  const WS_BIN_AUDIO_IN = 0x01;
  const WS_BIN_AUDIO_IN_END = 0x02;
  const WS_BIN_AUDIO_OUT = 0x11;
  const WS_BIN_AUDIO_SEGMENT_END = 0x12; // Play accumulated audio segment now

  // =============================================================================
  // State
  // =============================================================================
  let ws = null;
  let reconnectAttempts = 0;
  let currentState = 'idle';
  let debugMode = false;

  // Audio state
  let audioContext = null;
  let mediaStream = null;
  let audioProcessor = null;
  let isRecording = false;
  let audioSupported = false;

  // Audio playback state
  let playbackContext = null;
  let audioChunks = [];
  let isPlayingAudio = false;

  // Opus codec state
  let opusWorker = null;
  let opusReady = false;
  let capabilitiesSynced = false;  // True after server acknowledges our capabilities
  let pendingDecodes = [];      // Queue decoded audio until worker returns
  let pendingOpusData = [];     // Buffer for accumulating fragmented Opus data
  let pendingDecodePlayback = false;  // True when waiting for decode before playback

  // FFT visualization state (for playback - gives DAWN "life" when speaking)
  let playbackAnalyser = null;
  let fftAnimationId = null;
  let fftDataArray = null;
  let waveformHistory = [];  // Store recent waveform paths for trail effect
  let frameCount = 0;        // Frame counter for trail sampling
  const TRAIL_LENGTH = 5;    // Number of trailing echoes
  const TRAIL_SAMPLE_RATE = 10;  // Store trail every N frames for visible separation

  // Visualization mode: 'waveform' (smooth curve) or 'bars' (radial bar graph)
  let visualizationMode = 'bars';  // Switch to toggle between modes

  // Bar visualization state (radial EQ style)
  let barElements = null;      // Array of 128 line elements for current frame
  let barTrailElements = [];   // Array of 5 arrays (trails), each with 128 lines
  let barDataHistory = [];     // Store recent processed data for trails

  // FFT debug state
  let fftDebugState = {
    enabled: false,
    peakMax: 0
  };

  // LLM streaming state (ChatGPT-style real-time text)
  let streamingState = {
    active: false,
    streamId: null,
    entryElement: null,  // DOM element for current streaming entry
    textElement: null,   // Text container within entry
    content: '',         // Accumulated text content
    // Debounce markdown rendering (max 10Hz to avoid heavyweight parsing per token)
    lastRenderMs: 0,
    renderDebounceMs: 100,
    pendingRender: false
  };

  // Real-time metrics state (for multi-ring visualization)
  let metricsState = {
    state: 'idle',
    ttft_ms: 0,
    token_rate: 0,
    context_percent: 0,
    lastUpdate: 0,
    // Last non-zero values (persist after streaming ends)
    last_ttft_ms: 0,
    last_token_rate: 0
  };

  // Session averages tracking (exponential moving average)
  let avgState = {
    tokenRate: { sum: 0, count: 0 },
    ttft: { sum: 0, count: 0 },
    latency: { sum: 0, count: 0 }
  };

  // Hesitation ring state (token timing variance visualization)
  // Uses Welford's online algorithm for O(1) variance calculation
  let hesitationState = {
    dtWindow: [],           // Rolling window of inter-token intervals (ms)
    windowSize: 16,         // Window size
    tPrevMs: 0,             // Previous token timestamp
    loadSmooth: 0,          // Smoothed load value (0-1)
    lastTokenMs: 0,         // Time of last token for idle decay
    animationId: null,      // Animation frame ID for hesitation updates
    // Welford's running statistics (avoids array allocations)
    runningSum: 0,          // Sum of values in window
    runningSumSq: 0         // Sum of squared values in window
  };


  // Load-based ring transition state
  let loadTransitionState = {
    highLoadStartMs: 0,     // When load first exceeded threshold
    sustainedThreshold: 0.4, // Load level considered "high"
    sustainedDelay: 500,    // How long load must stay high before middle ring reacts (ms)
    isMiddleStrained: false // Whether middle ring is showing strain
  };

  // =============================================================================
  // DOM Elements
  // =============================================================================
  const elements = {
    connectionStatus: document.getElementById('connection-status'),
    ringContainer: document.getElementById('ring-container'),
    ringSvg: document.getElementById('ring-svg'),  // Main SVG container
    ringFft: document.getElementById('ring-fft'),  // Inner ring: FFT audio visualization
    ringThroughput: document.getElementById('ring-throughput'),  // Middle ring: token rate
    ringHesitation: document.getElementById('ring-hesitation'),  // Outer ring: timing variance
    ringInner: document.getElementById('ring-inner'),  // Glowing core
    waveformPath: document.getElementById('waveform-path'),
    waveformTrails: [
      document.getElementById('waveform-trail-1'),
      document.getElementById('waveform-trail-2'),
      document.getElementById('waveform-trail-3'),
      document.getElementById('waveform-trail-4'),
      document.getElementById('waveform-trail-5'),
    ],
    statusDot: document.getElementById('status-dot'),
    statusText: document.getElementById('status-text'),
    transcript: document.getElementById('transcript'),
    textInput: document.getElementById('text-input'),
    sendBtn: document.getElementById('send-btn'),
    micBtn: document.getElementById('mic-btn'),
    debugBtn: document.getElementById('debug-btn'),
  };

  // Multi-ring configuration (viewBox is 240x240, center at 120,120)
  const WAVEFORM_CENTER = 120;  // Center of SVG viewBox

  // Inner ring (FFT audio) - scaled down to make room for outer rings
  const WAVEFORM_BASE_RADIUS = 60;  // Base circle radius (was 85)
  const WAVEFORM_SPIKE_HEIGHT = 15;  // Max spike height (max radius ~75)
  const WAVEFORM_POINTS = 64;  // Number of points around the circle

  // Bar visualization configuration (radial EQ style)
  const BAR_COUNT = 128;              // Number of bars around the circle
  const BAR_INNER_RADIUS = 58;        // Inner edge of bars
  const BAR_MAX_OUTER_RADIUS = 83;    // Max outer edge (few pixels from throughput ring at 87)
  const BAR_GAP_DEGREES = 0.5;        // Gap between bars in degrees
  // Colors: cyan (low intensity) to amber (high intensity)
  const BAR_COLOR_LOW = { r: 34, g: 211, b: 238 };   // #22d3ee (cyan)
  const BAR_COLOR_HIGH = { r: 245, g: 158, b: 11 };  // #f59e0b (amber)

  // Middle ring (throughput) - shows token rate as segmented arc
  const THROUGHPUT_RADIUS = 87;  // Match SVG base circle
  const THROUGHPUT_SEGMENTS = 64;

  // Outer ring (hesitation) - shows token timing variance
  const HESITATION_RADIUS = 107;  // Match SVG base circle
  const HESITATION_SEGMENTS = 64;

  // Segment cache to avoid DOM thrashing (WeakMap: container -> segment paths array)
  const segmentCache = new WeakMap();

  // =============================================================================
  // WebSocket Connection
  // =============================================================================
  function getWebSocketUrl() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${protocol}//${window.location.host}/ws`;
  }

  function connect() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      return;
    }

    updateConnectionStatus('connecting');

    try {
      ws = new WebSocket(getWebSocketUrl(), WS_SUBPROTOCOL);
    } catch (e) {
      console.error('Failed to create WebSocket:', e);
      scheduleReconnect();
      return;
    }

    ws.binaryType = 'arraybuffer';

    ws.onopen = function() {
      console.log('WebSocket connected');
      reconnectAttempts = 0;
      updateConnectionStatus('connected');

      // Build capabilities for audio codec negotiation
      const capabilities = {
        audio_codecs: opusReady ? ['opus', 'pcm'] : ['pcm']
      };

      // Try to reconnect with existing session token, or request new session
      const savedToken = localStorage.getItem('dawn_session_token');
      if (savedToken) {
        console.log('Attempting session reconnect with token:', savedToken.substring(0, 8) + '...');
        ws.send(JSON.stringify({
          type: 'reconnect',
          payload: { token: savedToken, capabilities: capabilities }
        }));
      } else {
        // No saved token - request a new session
        console.log('No saved token, requesting new session');
        ws.send(JSON.stringify({
          type: 'init',
          payload: { capabilities: capabilities }
        }));
      }
      console.log('Audio codecs:', capabilities.audio_codecs);
    };

    ws.onclose = function(event) {
      console.log('WebSocket closed:', event.code, event.reason);
      capabilitiesSynced = false;  // Reset on disconnect
      // Show close reason if available (e.g., "max clients reached")
      if (event.reason) {
        updateConnectionStatus('disconnected', event.reason);
      } else {
        updateConnectionStatus('disconnected');
      }
      scheduleReconnect();
    };

    ws.onerror = function(error) {
      console.error('WebSocket error:', error);
    };

    ws.onmessage = function(event) {
      if (event.data instanceof ArrayBuffer) {
        handleBinaryMessage(event.data);
      } else {
        handleTextMessage(event.data);
      }
    };
  }

  function scheduleReconnect() {
    const delay = Math.min(
      RECONNECT_BASE_DELAY * Math.pow(2, reconnectAttempts),
      RECONNECT_MAX_DELAY
    );
    const jitter = Math.random() * 500;

    console.log(`Reconnecting in ${Math.round(delay + jitter)}ms...`);
    reconnectAttempts++;

    setTimeout(connect, delay + jitter);
  }

  function disconnect() {
    if (ws) {
      ws.close();
      ws = null;
    }
  }

  // =============================================================================
  // Message Handling
  // =============================================================================
  function handleTextMessage(data) {
    try {
      const msg = JSON.parse(data);
      console.log('Received:', msg);

      switch (msg.type) {
        case 'state':
          updateState(msg.payload.state, msg.payload.detail);
          break;
        case 'transcript':
          // Check for special LLM state update (sent with role '__llm_state__')
          if (msg.payload.role === '__llm_state__') {
            try {
              const stateMsg = JSON.parse(msg.payload.text);
              if (stateMsg.type === 'llm_state_update' && stateMsg.payload) {
                console.log('LLM state update received:', stateMsg.payload);
                updateLlmControls(stateMsg.payload);
              }
            } catch (e) {
              console.error('Failed to parse LLM state update:', e);
            }
          } else {
            addTranscriptEntry(msg.payload.role, msg.payload.text);
          }
          break;
        case 'error':
          console.error('Server error:', msg.payload);
          addTranscriptEntry('system', `Error: ${msg.payload.message}`);
          break;
        case 'session':
          console.log('Session token received');
          localStorage.setItem('dawn_session_token', msg.payload.token);
          // Server has processed our init/reconnect with capabilities
          capabilitiesSynced = true;
          break;
        case 'config':
          console.log('Config received:', msg.payload);
          if (msg.payload.audio_chunk_ms) {
            audioChunkMs = msg.payload.audio_chunk_ms;
            console.log('Audio chunk size set to:', audioChunkMs, 'ms');
          }
          break;
        case 'get_config_response':
          handleGetConfigResponse(msg.payload);
          break;
        case 'set_config_response':
          handleSetConfigResponse(msg.payload);
          break;
        case 'set_secrets_response':
          handleSetSecretsResponse(msg.payload);
          break;
        case 'get_audio_devices_response':
          handleGetAudioDevicesResponse(msg.payload);
          break;
        case 'list_models_response':
          handleModelsListResponse(msg.payload);
          break;
        case 'list_interfaces_response':
          handleInterfacesListResponse(msg.payload);
          break;
        case 'restart_response':
          handleRestartResponse(msg.payload);
          break;
        case 'set_session_llm_response':
          handleSetSessionLlmResponse(msg.payload);
          break;
        case 'smartthings_status_response':
          handleSmartThingsStatusResponse(msg.payload);
          break;
        case 'smartthings_auth_url_response':
          handleSmartThingsAuthUrlResponse(msg.payload);
          break;
        case 'smartthings_exchange_code_response':
          handleSmartThingsExchangeCodeResponse(msg.payload);
          break;
        case 'smartthings_disconnect_response':
          handleSmartThingsDisconnectResponse(msg.payload);
          break;
        case 'smartthings_devices_response':
          handleSmartThingsDevicesResponse(msg.payload);
          break;
        case 'system_prompt_response':
          handleSystemPromptResponse(msg.payload);
          break;
        case 'get_tools_config_response':
          handleGetToolsConfigResponse(msg.payload);
          break;
        case 'set_tools_config_response':
          handleSetToolsConfigResponse(msg.payload);
          break;
        case 'context':
          updateContextDisplay(msg.payload);
          break;
        case 'get_metrics_response':
          handleMetricsResponse(msg.payload);
          break;
        case 'stream_start':
          handleStreamStart(msg.payload);
          break;
        case 'stream_delta':
          handleStreamDelta(msg.payload);
          break;
        case 'stream_end':
          handleStreamEnd(msg.payload);
          break;
        case 'metrics_update':
          handleMetricsUpdate(msg.payload);
          break;
        default:
          console.log('Unknown message type:', msg.type);
      }
    } catch (e) {
      console.error('Failed to parse message:', e, data);
    }
  }

  function handleBinaryMessage(data) {
    try {
      if (data.byteLength < 1) {
        console.warn('Empty binary message');
        return;
      }

      const bytes = new Uint8Array(data);
      const msgType = bytes[0];
      console.log('Binary message: type=0x' + msgType.toString(16) + ', len=' + bytes.length);

      switch (msgType) {
        case WS_BIN_AUDIO_OUT:
          // TTS audio chunk - accumulate until segment end
          if (bytes.length > 1) {
            const payload = bytes.slice(1);
            // Store raw data (Opus or PCM) until segment end
            pendingOpusData.push(payload);
          }
          break;

        case WS_BIN_AUDIO_SEGMENT_END:
          // End of TTS audio segment - decode and play accumulated data
          if (pendingOpusData.length > 0) {
            // Concatenate all pending data
            const totalLen = pendingOpusData.reduce((sum, chunk) => sum + chunk.length, 0);
            const combined = new Uint8Array(totalLen);
            let offset = 0;
            for (const chunk of pendingOpusData) {
              combined.set(chunk, offset);
              offset += chunk.length;
            }
            pendingOpusData = [];

            if (opusReady && opusWorker) {
              // Decode complete Opus stream via worker
              // Set flag so playback triggers when decode completes
              pendingDecodePlayback = true;
              opusWorker.postMessage({ type: 'decode', data: combined }, [combined.buffer]);
            } else {
              // Raw PCM: 16-bit signed, 16kHz, mono
              audioChunks.push(combined);
              // Play immediately for PCM
              if (audioChunks.length > 0) {
                playAccumulatedAudio();
              }
            }
          }
          break;

        default:
          console.log('Unknown binary message type:', '0x' + msgType.toString(16));
      }
    } catch (e) {
      console.error('Error handling binary message:', e);
    }
  }

  // Audio playback queue - stores complete audio buffers waiting to play
  let audioPlaybackQueue = [];

  /**
   * Play accumulated PCM audio chunks via Web Audio API
   * Format: 16-bit signed integer PCM, 16kHz, mono
   * If audio is currently playing, queues the new audio for later
   */
  async function playAccumulatedAudio() {
    if (audioChunks.length === 0) {
      console.log('playAccumulatedAudio: no chunks to play');
      return;
    }

    // Concatenate chunks into a single buffer
    const totalLength = audioChunks.reduce((sum, chunk) => sum + chunk.length, 0);
    console.log('Total audio bytes:', totalLength);

    // Ensure even number of bytes for Int16 alignment
    const alignedLength = totalLength - (totalLength % 2);
    if (alignedLength !== totalLength) {
      console.warn('Audio length not aligned, truncating', totalLength - alignedLength, 'bytes');
    }

    const combinedBuffer = new Uint8Array(alignedLength);
    let offset = 0;
    for (const chunk of audioChunks) {
      const bytesToCopy = Math.min(chunk.length, alignedLength - offset);
      if (bytesToCopy > 0) {
        combinedBuffer.set(chunk.subarray(0, bytesToCopy), offset);
        offset += bytesToCopy;
      }
    }
    audioChunks = [];  // Clear for next audio stream

    if (isPlayingAudio) {
      // Queue this audio to play after current finishes
      console.log('Audio playing, queuing', alignedLength, 'bytes for later (queue size:', audioPlaybackQueue.length + 1, ')');
      audioPlaybackQueue.push(combinedBuffer);
      return;
    }

    // Play immediately
    await playAudioBuffer(combinedBuffer);
  }

  /**
   * Play a raw PCM buffer
   */
  async function playAudioBuffer(buffer) {
    isPlayingAudio = true;
    const alignedLength = buffer.length;

    try {
      // Convert bytes to Int16 samples (little-endian)
      const numSamples = alignedLength / 2;
      const int16Data = new Int16Array(numSamples);
      const dataView = new DataView(buffer.buffer);
      for (let i = 0; i < numSamples; i++) {
        int16Data[i] = dataView.getInt16(i * 2, true);  // true = little-endian
      }

      console.log('Playing', numSamples, 'samples (', (numSamples / AUDIO_SAMPLE_RATE).toFixed(2), 'seconds)');

      // Create playback AudioContext if needed
      const AudioContext = window.AudioContext || window.webkitAudioContext;
      if (!playbackContext || playbackContext.state === 'closed') {
        playbackContext = new AudioContext({ sampleRate: AUDIO_SAMPLE_RATE });

        // Create analyser for FFT visualization (gives DAWN "life" when speaking)
        playbackAnalyser = playbackContext.createAnalyser();
        playbackAnalyser.fftSize = 256;  // Frequency bins for detail
        playbackAnalyser.smoothingTimeConstant = 0.4;  // Lower = more responsive, higher = smoother
        playbackAnalyser.minDecibels = -55;  // Match TTS quiet parts (~-50dB)
        playbackAnalyser.maxDecibels = -10;  // Match TTS peaks (~-10dB)
        fftDataArray = new Uint8Array(playbackAnalyser.frequencyBinCount);
      }

      // Resume context if suspended (browser autoplay policy)
      if (playbackContext.state === 'suspended') {
        console.log('Resuming suspended audio context...');
        await playbackContext.resume();
      }

      // Create AudioBuffer
      const audioBuffer = playbackContext.createBuffer(1, numSamples, AUDIO_SAMPLE_RATE);
      const channelData = audioBuffer.getChannelData(0);

      // Convert Int16 (-32768 to 32767) to Float32 (-1.0 to 1.0)
      for (let i = 0; i < numSamples; i++) {
        channelData[i] = int16Data[i] / 32768.0;
      }

      // Create source node and connect through analyser for FFT visualization
      const source = playbackContext.createBufferSource();
      source.buffer = audioBuffer;
      source.connect(playbackAnalyser);
      playbackAnalyser.connect(playbackContext.destination);

      // Start FFT visualization if not already running
      if (!fftAnimationId) {
        startFFTVisualization();
      }

      source.onended = function() {
        console.log('Audio chunk finished');

        // Check if there's queued audio to play next
        if (audioPlaybackQueue.length > 0) {
          const nextBuffer = audioPlaybackQueue.shift();
          console.log('Playing next queued audio (remaining in queue:', audioPlaybackQueue.length, ')');
          playAudioBuffer(nextBuffer);
        } else {
          // No more audio - stop FFT and mark playback complete
          console.log('All audio playback finished');
          isPlayingAudio = false;
          stopFFTVisualization();
        }
      };

      console.log('Starting audio playback...');
      source.start(0);

    } catch (e) {
      console.error('Failed to play audio:', e);
      isPlayingAudio = false;
      stopFFTVisualization();
      // Try to play next in queue even on error
      if (audioPlaybackQueue.length > 0) {
        const nextBuffer = audioPlaybackQueue.shift();
        playAudioBuffer(nextBuffer);
      }
    }
  }

  // =============================================================================
  // Audio Capture
  // =============================================================================

  /**
   * Initialize audio context and check for microphone support
   */
  async function initAudio() {
    try {
      // Check for secure context (required for getUserMedia)
      if (!window.isSecureContext) {
        console.warn('Audio requires secure context (HTTPS or localhost)');
        console.warn('Current origin:', window.location.origin);
        return { supported: false, reason: 'Requires HTTPS or localhost access' };
      }

      // Check for Web Audio API support
      const AudioContext = window.AudioContext || window.webkitAudioContext;
      if (!AudioContext) {
        console.warn('Web Audio API not supported');
        return { supported: false, reason: 'Web Audio API not supported' };
      }

      // Check for getUserMedia support
      if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
        console.warn('getUserMedia not supported');
        return { supported: false, reason: 'getUserMedia not supported' };
      }

      audioSupported = true;
      console.log('Audio capture supported');
      return { supported: true, reason: null };
    } catch (e) {
      console.error('Audio init failed:', e);
      return { supported: false, reason: e.message };
    }
  }

  /**
   * Start recording audio from microphone
   */
  async function startRecording() {
    if (isRecording) {
      console.warn('Already recording');
      return;
    }

    if (!audioSupported) {
      console.error('Audio not supported');
      return;
    }

    try {
      // Request microphone access
      mediaStream = await navigator.mediaDevices.getUserMedia({
        audio: {
          sampleRate: AUDIO_SAMPLE_RATE,
          channelCount: AUDIO_CHANNELS,
          echoCancellation: true,
          noiseSuppression: true,
          autoGainControl: true
        }
      });

      // Create audio context at target sample rate
      const AudioContext = window.AudioContext || window.webkitAudioContext;
      audioContext = new AudioContext({ sampleRate: AUDIO_SAMPLE_RATE });
      console.log('AudioContext created: requested', AUDIO_SAMPLE_RATE, 'Hz, actual', audioContext.sampleRate, 'Hz');

      // Create source from microphone stream
      const source = audioContext.createMediaStreamSource(mediaStream);

      // Create ScriptProcessorNode for audio processing
      // Note: ScriptProcessorNode is deprecated but AudioWorklet requires HTTPS
      // Buffer size must be power of two, calculate from configured chunk ms
      const desiredSamples = Math.floor(AUDIO_SAMPLE_RATE * audioChunkMs / 1000);
      const bufferSize = Math.pow(2, Math.ceil(Math.log2(desiredSamples)));
      console.log('Audio buffer size:', bufferSize, 'samples (', bufferSize / AUDIO_SAMPLE_RATE * 1000, 'ms)');
      audioProcessor = audioContext.createScriptProcessor(bufferSize, 1, 1);

      audioProcessor.onaudioprocess = function(e) {
        // Clear output buffer to prevent feedback/garbage
        const outputData = e.outputBuffer.getChannelData(0);
        outputData.fill(0);

        if (!isRecording) return;

        // Get input audio data (Float32Array)
        const inputData = e.inputBuffer.getChannelData(0);

        // Convert Float32 (-1 to 1) to Int16 (-32768 to 32767)
        const pcmData = new Int16Array(inputData.length);
        for (let i = 0; i < inputData.length; i++) {
          const s = Math.max(-1, Math.min(1, inputData[i]));
          pcmData[i] = s < 0 ? s * 0x8000 : s * 0x7FFF;
        }

        // Send audio chunk to server
        sendAudioChunk(pcmData.buffer);
      };

      // Connect nodes: source -> processor -> destination
      source.connect(audioProcessor);
      audioProcessor.connect(audioContext.destination);

      isRecording = true;
      updateMicButton(true);
      console.log('Recording started');

    } catch (e) {
      console.error('Failed to start recording:', e);
      if (e.name === 'NotAllowedError') {
        addTranscriptEntry('system', 'Microphone access denied. Please allow microphone access.');
      } else {
        addTranscriptEntry('system', 'Failed to access microphone: ' + e.message);
      }
    }
  }

  /**
   * Stop recording and send end marker
   */
  function stopRecording() {
    if (!isRecording) {
      return;
    }

    isRecording = false;

    // Stop audio processing
    if (audioProcessor) {
      audioProcessor.disconnect();
      audioProcessor = null;
    }

    // Close audio context
    if (audioContext && audioContext.state !== 'closed') {
      audioContext.close();
      audioContext = null;
    }

    // Stop microphone stream
    if (mediaStream) {
      mediaStream.getTracks().forEach(track => track.stop());
      mediaStream = null;
    }

    // Send end marker to server
    sendAudioEnd();

    updateMicButton(false);
    console.log('Recording stopped');
  }

  /**
   * Send audio chunk to server
   */
  function sendAudioChunk(pcmBuffer) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    // Only use Opus if both worker is ready AND server has acknowledged our capabilities
    // This prevents race condition where we send Opus before server knows we support it
    if (opusReady && opusWorker && capabilitiesSynced) {
      // Encode via Opus worker - it will call sendOpusData when ready
      const pcmData = new Int16Array(pcmBuffer);
      opusWorker.postMessage({ type: 'encode', data: pcmData }, [pcmData.buffer]);
    } else {
      // Send raw PCM: [type byte][PCM data]
      const payload = new Uint8Array(1 + pcmBuffer.byteLength);
      payload[0] = WS_BIN_AUDIO_IN;
      payload.set(new Uint8Array(pcmBuffer), 1);
      ws.send(payload.buffer);
    }
  }

  /**
   * Send audio end marker to server
   */
  function sendAudioEnd() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    const payload = new Uint8Array([WS_BIN_AUDIO_IN_END]);
    ws.send(payload.buffer);
    console.log('Sent audio end marker');
  }

  // =============================================================================
  // FFT Visualization (animates rings when DAWN speaks)
  // =============================================================================

  /**
   * Process FFT data for visualization
   * - Uses logarithmic frequency scaling (speech is in low frequencies)
   * - Normalizes to maximize dynamic range
   * - Creates symmetrical output for pleasing visuals
   * @param {Uint8Array} fftData - Raw FFT frequency data
   * @returns {Float32Array} Processed values (0-1) for each waveform point
   */
  function processFFTData(fftData) {
    const halfPoints = WAVEFORM_POINTS / 2;
    const processed = new Float32Array(WAVEFORM_POINTS);

    if (!fftData || fftData.length === 0) {
      return processed;  // All zeros
    }

    // Use only the useful frequency range (skip DC, focus on speech frequencies)
    // At 48kHz sample rate, voice content is 0-10kHz = ~20% of Nyquist (24kHz)
    const usableBins = Math.floor(fftData.length * 0.4);  // 0-10kHz range
    const startBin = 1;  // Skip DC component

    // Sample FFT bins with logarithmic distribution for half the points
    const halfValues = new Float32Array(halfPoints);
    let maxVal = 0;

    for (let i = 0; i < halfPoints; i++) {
      // Logarithmic mapping: more points for low frequencies
      const t = i / halfPoints;
      const logT = Math.pow(t, 0.6);  // Compress high frequencies
      const binIndex = startBin + Math.floor(logT * (usableBins - startBin));

      // Average a few neighboring bins for smoother result
      let sum = 0;
      let count = 0;
      for (let j = -1; j <= 1; j++) {
        const idx = binIndex + j;
        if (idx >= 0 && idx < fftData.length) {
          sum += fftData[idx];
          count++;
        }
      }
      halfValues[i] = count > 0 ? sum / count : 0;
      if (halfValues[i] > maxVal) maxVal = halfValues[i];
    }

    // Normalize to 0-1 range with some minimum threshold
    const threshold = 3;  // Minimum value to consider as signal (lowered for Opus)
    if (maxVal > threshold) {
      for (let i = 0; i < halfPoints; i++) {
        halfValues[i] = Math.max(0, (halfValues[i] - threshold)) / (maxVal - threshold);
        // Apply slight curve for more dramatic spikes
        halfValues[i] = Math.pow(halfValues[i], 0.7);
      }
    }

    // Create symmetrical waveform (mirror around the circle)
    for (let i = 0; i < halfPoints; i++) {
      processed[i] = halfValues[i];
      processed[WAVEFORM_POINTS - 1 - i] = halfValues[i];
    }

    return processed;
  }

  /**
   * Generate SVG path for circular waveform
   * @param {Float32Array|null} processedData - Processed FFT values (0-1) or null for default circle
   * @param {number} scale - Overall scale multiplier
   * @returns {string} SVG path d attribute
   */
  function generateWaveformPath(processedData, scale = 1.0) {
    const points = [];
    const numPoints = WAVEFORM_POINTS;

    for (let i = 0; i < numPoints; i++) {
      const angle = (i / numPoints) * Math.PI * 2 - Math.PI / 2;  // Start at top

      // Get processed value for this point (or 0 if no data)
      const value = (processedData && processedData.length > i) ? processedData[i] : 0;

      // Calculate radius with spike based on value
      const spikeHeight = value * WAVEFORM_SPIKE_HEIGHT * scale;
      const radius = (WAVEFORM_BASE_RADIUS + spikeHeight) * scale;

      // Convert polar to cartesian
      const x = WAVEFORM_CENTER + Math.cos(angle) * radius;
      const y = WAVEFORM_CENTER + Math.sin(angle) * radius;

      points.push({ x, y });
    }

    // Build SVG path - use smooth curves for nicer look
    if (points.length === 0) return '';

    let path = `M ${points[0].x.toFixed(1)} ${points[0].y.toFixed(1)}`;

    // Use quadratic curves for smooth spikes
    for (let i = 1; i < points.length; i++) {
      const curr = points[i];
      const prev = points[i - 1];
      // Control point at midpoint
      const cpX = (prev.x + curr.x) / 2;
      const cpY = (prev.y + curr.y) / 2;
      path += ` Q ${prev.x.toFixed(1)} ${prev.y.toFixed(1)} ${cpX.toFixed(1)} ${cpY.toFixed(1)}`;
    }

    // Close the path smoothly
    const last = points[points.length - 1];
    const first = points[0];
    const cpX = (last.x + first.x) / 2;
    const cpY = (last.y + first.y) / 2;
    path += ` Q ${last.x.toFixed(1)} ${last.y.toFixed(1)} ${cpX.toFixed(1)} ${cpY.toFixed(1)}`;
    path += ' Z';

    return path;
  }

  /**
   * Draw default circular waveform (idle state)
   */
  function drawDefaultWaveform() {
    if (visualizationMode === 'bars') {
      // Reset bars to minimum height
      drawDefaultBars();
    } else {
      // Draw circular waveform
      const path = generateWaveformPath(null, 1.0);
      if (elements.waveformPath) {
        elements.waveformPath.setAttribute('d', path);
      }
      // Clear trails
      for (const trail of elements.waveformTrails) {
        if (trail) {
          trail.setAttribute('d', path);  // Set to same circle for clean look
        }
      }
    }
    waveformHistory = [];
    barDataHistory = [];
    frameCount = 0;
  }

  /**
   * Draw default bar state (idle - minimum height bars)
   */
  function drawDefaultBars() {
    if (!barElements || barElements.length === 0) return;

    const anglePerBar = 360 / BAR_COUNT;
    const minBarLength = BAR_INNER_RADIUS + 2;  // Just a tiny bit visible

    for (let i = 0; i < BAR_COUNT; i++) {
      const angleDeg = i * anglePerBar - 90;
      const angleRad = angleDeg * Math.PI / 180;

      const x2 = WAVEFORM_CENTER + Math.cos(angleRad) * minBarLength;
      const y2 = WAVEFORM_CENTER + Math.sin(angleRad) * minBarLength;

      barElements[i].setAttribute('x2', x2.toFixed(1));
      barElements[i].setAttribute('y2', y2.toFixed(1));
      barElements[i].setAttribute('stroke', interpolateBarColor(0));
    }

    // Reset trail bars too
    for (let t = 0; t < barTrailElements.length; t++) {
      for (let i = 0; i < BAR_COUNT; i++) {
        const angleDeg = i * anglePerBar - 90;
        const angleRad = angleDeg * Math.PI / 180;
        const x2 = WAVEFORM_CENTER + Math.cos(angleRad) * minBarLength;
        const y2 = WAVEFORM_CENTER + Math.sin(angleRad) * minBarLength;
        barTrailElements[t][i].setAttribute('x2', x2.toFixed(1));
        barTrailElements[t][i].setAttribute('y2', y2.toFixed(1));
      }
    }
  }

  /**
   * Generate a segmented arc as SVG path elements
   * @param {Element} container - SVG group element to add segments to
   * @param {number} radius - Arc radius
   * @param {number} segments - Number of segments
   * @param {number} fillLevel - 0-1 how much of the arc is "active"
   * @param {number[]} jitter - Optional per-segment radius jitter array
   */
  function renderSegmentedArc(container, radius, segments, fillLevel = 0, jitter = null) {
    if (!container) return;

    const gapAngle = 0.02;  // Gap between segments in radians
    const segmentAngle = (Math.PI * 2 / segments) - gapAngle;
    const activeSegments = Math.floor(segments * fillLevel);

    // Get or create cached segment paths - avoids DOM thrashing
    let paths = segmentCache.get(container);
    if (!paths || paths.length !== segments) {
      // First call or segment count changed - create elements once
      while (container.firstChild) {
        container.removeChild(container.firstChild);
      }
      paths = [];
      for (let i = 0; i < segments; i++) {
        const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
        path.setAttribute('class', 'ring-segment');
        path.setAttribute('fill', 'none');
        container.appendChild(path);
        paths.push(path);
      }
      segmentCache.set(container, paths);
    }

    // Update existing segments - O(n) attribute updates, no DOM creation
    for (let i = 0; i < segments; i++) {
      const startAngle = (i / segments) * Math.PI * 2 - Math.PI / 2;
      const endAngle = startAngle + segmentAngle;

      // Apply jitter if provided
      const r = jitter && jitter[i] ? radius + jitter[i] : radius;

      // Calculate arc points
      const x1 = WAVEFORM_CENTER + Math.cos(startAngle) * r;
      const y1 = WAVEFORM_CENTER + Math.sin(startAngle) * r;
      const x2 = WAVEFORM_CENTER + Math.cos(endAngle) * r;
      const y2 = WAVEFORM_CENTER + Math.sin(endAngle) * r;

      // Update path attributes only - no element creation
      paths[i].setAttribute('d', `M ${x1.toFixed(1)} ${y1.toFixed(1)} A ${r} ${r} 0 0 1 ${x2.toFixed(1)} ${y2.toFixed(1)}`);
      paths[i].style.opacity = i < activeSegments ? '0.8' : '0.22';
    }
  }

  /**
   * Initialize the multi-ring structure with default states
   */
  function initializeRings() {
    // Draw throughput ring (middle) - starts empty
    renderSegmentedArc(elements.ringThroughput, THROUGHPUT_RADIUS, THROUGHPUT_SEGMENTS, 0);

    // Draw hesitation ring (outer) - starts at baseline
    renderSegmentedArc(elements.ringHesitation, HESITATION_RADIUS, HESITATION_SEGMENTS, 0);

    // Initialize bar visualization elements if in bar mode
    if (visualizationMode === 'bars') {
      initializeBarElements();
    }
  }

  // =============================================================================
  // Bar Visualization (Radial EQ Style)
  // =============================================================================

  /**
   * Interpolate between two colors based on intensity (0-1)
   */
  function interpolateBarColor(intensity) {
    const t = Math.max(0, Math.min(1, intensity));
    const r = Math.round(BAR_COLOR_LOW.r + (BAR_COLOR_HIGH.r - BAR_COLOR_LOW.r) * t);
    const g = Math.round(BAR_COLOR_LOW.g + (BAR_COLOR_HIGH.g - BAR_COLOR_LOW.g) * t);
    const b = Math.round(BAR_COLOR_LOW.b + (BAR_COLOR_HIGH.b - BAR_COLOR_LOW.b) * t);
    return `rgb(${r},${g},${b})`;
  }

  /**
   * Create a single bar line element
   */
  function createBarElement(container, index, isTrail = false, trailIndex = 0) {
    const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    line.setAttribute('stroke-linecap', 'butt');

    // Calculate angle for this bar (distribute evenly around circle)
    const anglePerBar = 360 / BAR_COUNT;
    const gapAngle = BAR_GAP_DEGREES;
    const barAngle = anglePerBar - gapAngle;

    // Bar width is determined by the arc length at the outer radius
    // stroke-width approximates the visual thickness
    const circumference = 2 * Math.PI * BAR_MAX_OUTER_RADIUS;
    const barWidth = (circumference / BAR_COUNT) - 1;  // -1 for gap
    line.setAttribute('stroke-width', Math.max(1, barWidth * 0.85).toFixed(1));

    // Set initial position (will be updated during animation)
    const angleDeg = index * anglePerBar - 90;  // Start from top
    const angleRad = angleDeg * Math.PI / 180;

    const x1 = WAVEFORM_CENTER + Math.cos(angleRad) * BAR_INNER_RADIUS;
    const y1 = WAVEFORM_CENTER + Math.sin(angleRad) * BAR_INNER_RADIUS;
    const x2 = WAVEFORM_CENTER + Math.cos(angleRad) * BAR_INNER_RADIUS;  // Start collapsed
    const y2 = WAVEFORM_CENTER + Math.sin(angleRad) * BAR_INNER_RADIUS;

    line.setAttribute('x1', x1.toFixed(1));
    line.setAttribute('y1', y1.toFixed(1));
    line.setAttribute('x2', x2.toFixed(1));
    line.setAttribute('y2', y2.toFixed(1));
    line.setAttribute('stroke', `rgb(${BAR_COLOR_LOW.r},${BAR_COLOR_LOW.g},${BAR_COLOR_LOW.b})`);

    // Trail elements have decreasing opacity
    if (isTrail) {
      line.style.opacity = (0.6 - trailIndex * 0.1).toFixed(2);
      line.classList.add('bar-trail');
    } else {
      line.style.opacity = '0.9';
      line.classList.add('bar-current');
    }

    container.appendChild(line);
    return line;
  }

  /**
   * Initialize all bar elements (current + trails)
   */
  function initializeBarElements() {
    const container = elements.ringFft;
    if (!container) return;

    // Clear existing waveform paths (hide them, don't remove)
    if (elements.waveformPath) {
      elements.waveformPath.style.display = 'none';
    }
    elements.waveformTrails.forEach(trail => {
      if (trail) trail.style.display = 'none';
    });

    // Create group for bar elements
    let barGroup = container.querySelector('.bar-group');
    if (!barGroup) {
      barGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
      barGroup.classList.add('bar-group');
      container.appendChild(barGroup);
    } else {
      barGroup.innerHTML = '';  // Clear existing bars
    }

    // Create trail elements first (so they render behind current)
    barTrailElements = [];
    for (let t = TRAIL_LENGTH - 1; t >= 0; t--) {
      const trailBars = [];
      for (let i = 0; i < BAR_COUNT; i++) {
        trailBars.push(createBarElement(barGroup, i, true, t));
      }
      barTrailElements.unshift(trailBars);  // Oldest trails first
    }

    // Create current frame bar elements (on top)
    barElements = [];
    for (let i = 0; i < BAR_COUNT; i++) {
      barElements.push(createBarElement(barGroup, i, false));
    }

    // Initialize data history
    barDataHistory = [];

    console.log(`Initialized ${BAR_COUNT} bar elements with ${TRAIL_LENGTH} trail layers`);
  }

  /**
   * Process FFT data for bar visualization (128 bars from frequency bins)
   * Uses same tuning as waveform but spreads across all bars (no mirror)
   */
  function processFFTDataForBars(fftData) {
    const processed = new Float32Array(BAR_COUNT);

    if (!fftData || fftData.length === 0) {
      return processed;
    }

    // Use only the useful frequency range (skip DC, focus on speech frequencies)
    // Same tuning as processFFTData - 0-10kHz range for voice content
    const usableBins = Math.floor(fftData.length * 0.4);
    const startBin = 1;  // Skip DC component

    let maxVal = 0;

    // Map all 128 bars across the usable frequency range
    for (let i = 0; i < BAR_COUNT; i++) {
      // Logarithmic mapping: more bars for low frequencies (same 0.6 power as waveform)
      const t = i / BAR_COUNT;
      const logT = Math.pow(t, 0.6);
      const binIndex = startBin + Math.floor(logT * (usableBins - startBin));

      // Average neighboring bins for smoother result
      let sum = 0;
      let count = 0;
      for (let j = -1; j <= 1; j++) {
        const idx = binIndex + j;
        if (idx >= 0 && idx < fftData.length) {
          sum += fftData[idx];
          count++;
        }
      }
      processed[i] = count > 0 ? sum / count : 0;
      if (processed[i] > maxVal) maxVal = processed[i];
    }

    // Normalize to 0-1 range with threshold (same as waveform)
    const threshold = 3;
    if (maxVal > threshold) {
      for (let i = 0; i < BAR_COUNT; i++) {
        processed[i] = Math.max(0, (processed[i] - threshold)) / (maxVal - threshold);
        // Apply curve for more dramatic spikes (same 0.7 power as waveform)
        processed[i] = Math.pow(processed[i], 0.7);
      }
    }

    return processed;
  }

  /**
   * Update bar elements with current FFT data
   */
  function renderBars(processedData, scale = 1.0) {
    if (!barElements || barElements.length === 0) return;

    const anglePerBar = 360 / BAR_COUNT;

    for (let i = 0; i < BAR_COUNT; i++) {
      const value = processedData[i] || 0;
      const angleDeg = i * anglePerBar - 90;  // Start from top
      const angleRad = angleDeg * Math.PI / 180;

      // Calculate bar length based on value
      const barLength = BAR_INNER_RADIUS + value * (BAR_MAX_OUTER_RADIUS - BAR_INNER_RADIUS) * scale;

      // Inner point (fixed)
      const x1 = WAVEFORM_CENTER + Math.cos(angleRad) * BAR_INNER_RADIUS;
      const y1 = WAVEFORM_CENTER + Math.sin(angleRad) * BAR_INNER_RADIUS;

      // Outer point (varies with amplitude)
      const x2 = WAVEFORM_CENTER + Math.cos(angleRad) * barLength;
      const y2 = WAVEFORM_CENTER + Math.sin(angleRad) * barLength;

      // Update element
      barElements[i].setAttribute('x2', x2.toFixed(1));
      barElements[i].setAttribute('y2', y2.toFixed(1));

      // Color based on intensity
      barElements[i].setAttribute('stroke', interpolateBarColor(value));
    }
  }

  /**
   * Update bar trail elements with historical data
   */
  function renderBarTrails() {
    for (let t = 0; t < barTrailElements.length && t < barDataHistory.length; t++) {
      const trailData = barDataHistory[t];
      const trailBars = barTrailElements[t];
      const anglePerBar = 360 / BAR_COUNT;

      // Trail opacity decreases with age
      const baseOpacity = 0.5 - t * 0.08;

      for (let i = 0; i < BAR_COUNT; i++) {
        const value = trailData[i] || 0;
        const angleDeg = i * anglePerBar - 90;
        const angleRad = angleDeg * Math.PI / 180;

        const barLength = BAR_INNER_RADIUS + value * (BAR_MAX_OUTER_RADIUS - BAR_INNER_RADIUS);

        const x2 = WAVEFORM_CENTER + Math.cos(angleRad) * barLength;
        const y2 = WAVEFORM_CENTER + Math.sin(angleRad) * barLength;

        trailBars[i].setAttribute('x2', x2.toFixed(1));
        trailBars[i].setAttribute('y2', y2.toFixed(1));
        trailBars[i].setAttribute('stroke', interpolateBarColor(value * 0.7));  // Slightly dimmer color
        trailBars[i].style.opacity = (baseOpacity * (1 - value * 0.3)).toFixed(2);
      }
    }
  }

  /**
   * Show waveform elements, hide bar elements
   */
  function showWaveformMode() {
    // Show waveform paths
    if (elements.waveformPath) {
      elements.waveformPath.style.display = '';
    }
    elements.waveformTrails.forEach(trail => {
      if (trail) trail.style.display = '';
    });

    // Hide bar group
    const barGroup = elements.ringFft?.querySelector('.bar-group');
    if (barGroup) {
      barGroup.style.display = 'none';
    }
  }

  /**
   * Show bar elements, hide waveform elements
   */
  function showBarMode() {
    // Hide waveform paths
    if (elements.waveformPath) {
      elements.waveformPath.style.display = 'none';
    }
    elements.waveformTrails.forEach(trail => {
      if (trail) trail.style.display = 'none';
    });

    // Show bar group
    const barGroup = elements.ringFft?.querySelector('.bar-group');
    if (barGroup) {
      barGroup.style.display = '';
    }

    // Initialize bars if not already done
    if (!barElements || barElements.length === 0) {
      initializeBarElements();
    }
  }

  /**
   * Toggle between waveform and bar visualization modes
   */
  function toggleVisualizationMode() {
    visualizationMode = visualizationMode === 'waveform' ? 'bars' : 'waveform';

    if (visualizationMode === 'bars') {
      showBarMode();
    } else {
      showWaveformMode();
    }

    console.log('Visualization mode:', visualizationMode);
    return visualizationMode;
  }

  // =============================================================================
  // Hesitation Ring Algorithm (Phase 2A)
  // =============================================================================

  /**
   * Calculate standard deviation using running statistics (Welford's algorithm)
   * O(1) computation instead of O(n) - no array allocations
   */
  function getRunningStddev() {
    const n = hesitationState.dtWindow.length;
    if (n === 0) return 0;
    const mean = hesitationState.runningSum / n;
    const variance = (hesitationState.runningSumSq / n) - (mean * mean);
    return Math.sqrt(Math.max(0, variance));  // max(0,...) for numerical stability
  }

  /**
   * Smooth noise function for hesitation jitter
   * Uses sine waves at different frequencies for organic movement
   * @param {number} segmentId - Segment index (0-63)
   * @param {number} time - Current time in seconds
   * @returns {number} Noise value (-1 to 1)
   */
  function smoothNoise(segmentId, time) {
    // Use multiple sine waves at different phases for each segment
    const phase1 = segmentId * 0.37;  // Prime-ish multiplier for variation
    const phase2 = segmentId * 0.73;
    const phase3 = segmentId * 1.17;

    // Different frequencies for organic feel
    const n1 = Math.sin(time * 2.0 + phase1);
    const n2 = Math.sin(time * 3.7 + phase2) * 0.5;
    const n3 = Math.sin(time * 5.3 + phase3) * 0.25;

    return (n1 + n2 + n3) / 1.75;  // Normalize to roughly -1 to 1
  }

  /**
   * Handle token event for hesitation tracking
   * Call this when a stream_delta is received
   * Uses Welford's online algorithm for O(1) stddev updates
   */
  function onTokenEvent() {
    const nowMs = performance.now();

    if (hesitationState.tPrevMs > 0) {
      const dtMs = nowMs - hesitationState.tPrevMs;

      // Remove oldest value from running stats if window is full
      if (hesitationState.dtWindow.length >= hesitationState.windowSize) {
        const oldValue = hesitationState.dtWindow.shift();
        hesitationState.runningSum -= oldValue;
        hesitationState.runningSumSq -= oldValue * oldValue;
      }

      // Add new value to window and running stats
      hesitationState.dtWindow.push(dtMs);
      hesitationState.runningSum += dtMs;
      hesitationState.runningSumSq += dtMs * dtMs;

      // Calculate standard deviation using O(1) running stats
      const std = getRunningStddev();
      // 3ms = calm (fast, steady tokens), 20ms+ = stressed (slow, variable)
      // Lower thresholds for more sensitivity to chunk timing variance
      const loadRaw = (std - 3) / (20 - 3);
      const load = Math.max(0, Math.min(1, loadRaw));

      // EMA smoothing
      hesitationState.loadSmooth += 0.2 * (load - hesitationState.loadSmooth);
    }

    hesitationState.tPrevMs = nowMs;
    hesitationState.lastTokenMs = nowMs;
  }

  /**
   * Update hesitation ring with jitter based on load
   * Called from animation loop
   */
  function updateHesitationRing() {
    const nowMs = performance.now();
    const timeSec = nowMs / 1000;

    // Idle decay: if no token for 300ms, decay loadSmooth
    if (nowMs - hesitationState.lastTokenMs > 300) {
      hesitationState.loadSmooth *= 0.95;
      if (hesitationState.loadSmooth < 0.01) {
        hesitationState.loadSmooth = 0;
      }
    }

    // Generate jitter array for each segment
    const jitter = new Array(HESITATION_SEGMENTS);
    const maxJitter = 3;  // Max jitter in pixels

    for (let i = 0; i < HESITATION_SEGMENTS; i++) {
      const noise = smoothNoise(i, timeSec);
      jitter[i] = noise * maxJitter * hesitationState.loadSmooth;
    }

    // Render with jitter
    renderSegmentedArc(elements.ringHesitation, HESITATION_RADIUS, HESITATION_SEGMENTS, 1.0, jitter);

    // Load-based middle ring transition (outer reacts first, middle follows)
    updateMiddleRingStrain(nowMs);

    // Update telemetry panel latency display during streaming
    updateTelemetryPanel();
  }

  /**
   * Update middle ring strain state based on sustained high load
   * Per design: outer ring reacts first, middle ring follows after delay
   */
  function updateMiddleRingStrain(nowMs) {
    const load = hesitationState.loadSmooth;
    const threshold = loadTransitionState.sustainedThreshold;
    const delay = loadTransitionState.sustainedDelay;

    if (load > threshold) {
      // Load is high - track when it started
      if (loadTransitionState.highLoadStartMs === 0) {
        loadTransitionState.highLoadStartMs = nowMs;
      }

      // Check if sustained long enough
      if (nowMs - loadTransitionState.highLoadStartMs > delay) {
        if (!loadTransitionState.isMiddleStrained) {
          elements.ringThroughput.classList.add('strained');
          loadTransitionState.isMiddleStrained = true;
        }
      }
    } else {
      // Load dropped - reset tracking
      loadTransitionState.highLoadStartMs = 0;
      if (loadTransitionState.isMiddleStrained) {
        elements.ringThroughput.classList.remove('strained');
        loadTransitionState.isMiddleStrained = false;
      }
    }
  }

  /**
   * Start hesitation ring animation loop
   */
  function startHesitationAnimation() {
    if (hesitationState.animationId) return;  // Already running

    function animate() {
      updateHesitationRing();
      hesitationState.animationId = requestAnimationFrame(animate);
    }

    hesitationState.animationId = requestAnimationFrame(animate);
  }

  /**
   * Stop hesitation ring animation and reset
   */
  function stopHesitationAnimation() {
    if (hesitationState.animationId) {
      cancelAnimationFrame(hesitationState.animationId);
      hesitationState.animationId = null;
    }

    // Reset state
    hesitationState.dtWindow = [];
    hesitationState.tPrevMs = 0;
    hesitationState.loadSmooth = 0;

    // Render baseline ring
    renderSegmentedArc(elements.ringHesitation, HESITATION_RADIUS, HESITATION_SEGMENTS, 0);
  }


  /**
   * Start FFT visualization animation loop
   */
  function startFFTVisualization() {
    if (!elements.ringContainer || !playbackAnalyser || !fftDataArray) {
      console.warn('FFT visualization: missing requirements');
      return;
    }

    console.log('Starting FFT visualization for DAWN speech');

    // Add class to pause CSS animations and enable JS control
    elements.ringContainer.classList.add('fft-active');

    // Animation loop
    function animate() {
      if (!playbackAnalyser || !fftDataArray || !isPlayingAudio) {
        return;
      }

      // Get frequency data
      playbackAnalyser.getByteFrequencyData(fftDataArray);

      // Calculate volume stats for inner ring scaling and debug
      let sum = 0;
      let maxRaw = 0;
      for (let i = 0; i < fftDataArray.length; i++) {
        sum += fftDataArray[i];
        if (fftDataArray[i] > maxRaw) maxRaw = fftDataArray[i];
      }
      const average = sum / fftDataArray.length;
      const normalizedLevel = average / 255;

      // Update debug display if visible
      if (fftDebugState.enabled) {
        if (maxRaw > fftDebugState.peakMax) fftDebugState.peakMax = maxRaw;
        const dbgMax = document.getElementById('dbg-max');
        const dbgAvg = document.getElementById('dbg-avg');
        const dbgNorm = document.getElementById('dbg-norm');
        const dbgPeak = document.getElementById('dbg-peak');
        if (dbgMax) dbgMax.textContent = maxRaw;
        if (dbgAvg) dbgAvg.textContent = average.toFixed(0);
        if (dbgNorm) dbgNorm.textContent = normalizedLevel.toFixed(2);
        if (dbgPeak) dbgPeak.textContent = fftDebugState.peakMax;
      }

      // Render based on visualization mode
      if (visualizationMode === 'bars') {
        // Bar visualization (radial EQ style)
        const barData = processFFTDataForBars(fftDataArray);
        renderBars(barData, 1.0);

        // Update trail history every N frames for visible separation
        frameCount++;
        if (frameCount >= TRAIL_SAMPLE_RATE) {
          frameCount = 0;
          barDataHistory.unshift(Array.from(barData));
          if (barDataHistory.length > TRAIL_LENGTH) {
            barDataHistory.pop();
          }
        }

        // Render bar trails
        renderBarTrails();

      } else {
        // Waveform visualization (smooth curve)
        const processedData = processFFTData(fftDataArray);

        if (elements.waveformPath) {
          const path = generateWaveformPath(processedData, 1.0);
          elements.waveformPath.setAttribute('d', path);

          // Update trail history every N frames for visible separation
          frameCount++;
          if (frameCount >= TRAIL_SAMPLE_RATE) {
            frameCount = 0;
            waveformHistory.unshift(path);
            if (waveformHistory.length > TRAIL_LENGTH) {
              waveformHistory.pop();
            }
          }

          // Update trail paths with historical data
          for (let i = 0; i < elements.waveformTrails.length; i++) {
            if (elements.waveformTrails[i] && waveformHistory[i]) {
              elements.waveformTrails[i].setAttribute('d', waveformHistory[i]);
            }
          }
        }
      }

      // Time-based animations
      const time = Date.now() / 1000;

      // Pulse the glowing core based on audio intensity
      const coreScale = 1.0 + normalizedLevel * 0.25;
      const coreOpacity = 0.6 + normalizedLevel * 0.4;  // 0.6 to 1.0
      if (elements.ringInner) {
        elements.ringInner.style.transform = `scale(${coreScale.toFixed(3)})`;
        elements.ringInner.style.opacity = coreOpacity.toFixed(2);
      }

      // Per design spec: rings do NOT rotate or translate
      // Motion is only allowed for radial jitter, glow, or subtle idle breathing

      fftAnimationId = requestAnimationFrame(animate);
    }

    fftAnimationId = requestAnimationFrame(animate);
  }

  /**
   * Stop FFT visualization and restore CSS animations
   */
  function stopFFTVisualization() {
    if (fftAnimationId) {
      cancelAnimationFrame(fftAnimationId);
      fftAnimationId = null;
    }

    // Reset to default circular waveform
    drawDefaultWaveform();

    // Reset ring transforms and remove fft-active class
    if (elements.ringInner) {
      elements.ringInner.style.transform = '';
      elements.ringInner.style.opacity = '';  // Reset to CSS default
    }
    if (elements.ringContainer) {
      elements.ringContainer.classList.remove('fft-active');
    }
    console.log('Stopped FFT visualization');
  }

  /**
   * Update mic button visual state
   */
  function updateMicButton(recording) {
    if (elements.micBtn) {
      elements.micBtn.classList.toggle('recording', recording);
      elements.micBtn.textContent = recording ? 'Stop' : 'Mic';
      elements.micBtn.title = recording ? 'Stop recording' : 'Start recording';
    }
  }

  function sendTextMessage(text) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      return false;
    }

    const msg = {
      type: 'text',
      payload: { text: text }
    };

    ws.send(JSON.stringify(msg));
    // Note: Server echoes user message back as transcript, so no local entry needed
    // State update also comes from server
    return true;
  }

  // =============================================================================
  // UI Updates
  // =============================================================================
  function updateConnectionStatus(status, reason) {
    elements.connectionStatus.className = status;
    if (status === 'connected') {
      elements.connectionStatus.textContent = 'Connected';
    } else if (status === 'connecting') {
      elements.connectionStatus.textContent = 'Connecting...';
    } else {
      // Show disconnect reason if available (truncate for display)
      elements.connectionStatus.textContent = reason
        ? 'Disconnected: ' + (reason.length > 30 ? reason.substring(0, 30) + '...' : reason)
        : 'Disconnected';
    }
  }

  function updateState(state, detail) {
    const previousState = currentState;
    currentState = state;

    // Update status indicator
    elements.statusDot.className = state;

    // Show state with optional detail (e.g., "THINKING · Fetching URL...")
    if (detail) {
      elements.statusText.textContent = state.toUpperCase() + ' · ' + detail;
    } else {
      elements.statusText.textContent = state.toUpperCase();
    }

    // Update ring container - preserve fft-active class if present
    const hasFftActive = elements.ringContainer.classList.contains('fft-active');
    elements.ringContainer.classList.remove(previousState);
    elements.ringContainer.classList.add(state);
    // Re-add fft-active if it was there (in case it got removed)
    if (hasFftActive) {
      elements.ringContainer.classList.add('fft-active');
    }

    // Emit state event for decoupled modules
    if (typeof DawnEvents !== 'undefined') {
      DawnEvents.emit('state', { state, previousState, detail });
    }
  }

  // =============================================================================
  // Context Pressure Gauge (Phase 3) - Rainbow Arc Style
  // =============================================================================

  const GAUGE_SEGMENTS = 32;        // Number of arc segments
  const GAUGE_RADIUS = 75;          // Base radius of the arc
  const GAUGE_START_ANGLE = -165;   // Start angle (degrees, 0 = right, -90 = top)
  const GAUGE_END_ANGLE = -15;      // End angle (degrees)
  const GAUGE_GAP_DEG = 1.5;        // Gap between segments in degrees
  const GAUGE_MIN_THICKNESS = 3;    // Thickness at start (thin end)
  const GAUGE_MAX_THICKNESS = 22;   // Thickness at end (fat end)

  // Color interpolation for gauge (cyan to amber, matching audio bars)
  const GAUGE_COLOR_LOW = { r: 34, g: 211, b: 238 };   // Cyan #22d3ee
  const GAUGE_COLOR_HIGH = { r: 245, g: 158, b: 11 };  // Amber #f59e0b

  /**
   * Interpolate gauge color based on position (0-1)
   */
  function interpolateGaugeColor(t) {
    const r = Math.round(GAUGE_COLOR_LOW.r + (GAUGE_COLOR_HIGH.r - GAUGE_COLOR_LOW.r) * t);
    const g = Math.round(GAUGE_COLOR_LOW.g + (GAUGE_COLOR_HIGH.g - GAUGE_COLOR_LOW.g) * t);
    const b = Math.round(GAUGE_COLOR_LOW.b + (GAUGE_COLOR_HIGH.b - GAUGE_COLOR_LOW.b) * t);
    return `rgb(${r},${g},${b})`;
  }

  /**
   * Initialize context gauge segments - rainbow arc, thin to thick
   */
  function initializeContextGauge() {
    const container = document.getElementById('context-gauge-segments');
    if (!container) return;

    // Clear existing
    container.innerHTML = '';

    const totalArc = GAUGE_END_ANGLE - GAUGE_START_ANGLE;  // Total arc span
    const totalGaps = GAUGE_GAP_DEG * (GAUGE_SEGMENTS - 1);
    const segmentArc = (totalArc - totalGaps) / GAUGE_SEGMENTS;

    for (let i = 0; i < GAUGE_SEGMENTS; i++) {
      const t = i / (GAUGE_SEGMENTS - 1);  // 0 to 1 across gauge

      // Angle for this segment
      const startAngle = GAUGE_START_ANGLE + i * (segmentArc + GAUGE_GAP_DEG);
      const endAngle = startAngle + segmentArc;

      // Thickness increases from start to end (thin to fat)
      const thickness = GAUGE_MIN_THICKNESS + (GAUGE_MAX_THICKNESS - GAUGE_MIN_THICKNESS) * t;

      // Inner and outer radius for this segment
      const innerRadius = GAUGE_RADIUS - thickness / 2;
      const outerRadius = GAUGE_RADIUS + thickness / 2;

      // Convert to radians
      const startRad = startAngle * Math.PI / 180;
      const endRad = endAngle * Math.PI / 180;

      // Calculate arc points
      const x1Inner = Math.cos(startRad) * innerRadius;
      const y1Inner = Math.sin(startRad) * innerRadius;
      const x2Inner = Math.cos(endRad) * innerRadius;
      const y2Inner = Math.sin(endRad) * innerRadius;
      const x1Outer = Math.cos(startRad) * outerRadius;
      const y1Outer = Math.sin(startRad) * outerRadius;
      const x2Outer = Math.cos(endRad) * outerRadius;
      const y2Outer = Math.sin(endRad) * outerRadius;

      // Create path: outer arc, line to inner, inner arc back, close
      const largeArc = (endAngle - startAngle) > 180 ? 1 : 0;
      const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      path.setAttribute('class', 'gauge-segment');
      path.setAttribute('d', `
        M ${x1Outer.toFixed(2)} ${y1Outer.toFixed(2)}
        A ${outerRadius.toFixed(2)} ${outerRadius.toFixed(2)} 0 ${largeArc} 1 ${x2Outer.toFixed(2)} ${y2Outer.toFixed(2)}
        L ${x2Inner.toFixed(2)} ${y2Inner.toFixed(2)}
        A ${innerRadius.toFixed(2)} ${innerRadius.toFixed(2)} 0 ${largeArc} 0 ${x1Inner.toFixed(2)} ${y1Inner.toFixed(2)}
        Z
      `);

      // Store position info for color interpolation
      path.dataset.index = i;
      path.dataset.position = t.toFixed(3);

      // Set base color (dimmed) based on position
      const color = interpolateGaugeColor(t);
      path.setAttribute('fill', color);
      path.style.opacity = '0.2';  // Dimmed when inactive

      container.appendChild(path);
    }
  }

  /**
   * Update context gauge arc with current usage
   * @param {number} usage - Usage percentage (0-100)
   */
  function renderContextGauge(usage) {
    const container = document.getElementById('context-gauge-segments');
    if (!container) return;

    const segments = container.querySelectorAll('.gauge-segment');
    const activeCount = Math.ceil((usage / 100) * GAUGE_SEGMENTS);

    segments.forEach((seg, i) => {
      const t = parseFloat(seg.dataset.position) || (i / (GAUGE_SEGMENTS - 1));
      const color = interpolateGaugeColor(t);

      seg.classList.remove('flashing');

      if (i < activeCount) {
        // Active segment - full brightness, no glow (was bleeding onto text)
        seg.style.opacity = '0.9';
        seg.setAttribute('fill', color);
        seg.style.filter = 'none';

        if (t > 0.9) {
          // Critical level - red tint and flash
          seg.setAttribute('fill', '#ef4444');
          if (usage > 90) {
            seg.classList.add('flashing');
          }
        }
      } else {
        // Inactive segment - dimmed, show underlying color
        seg.style.opacity = '0.15';
        seg.setAttribute('fill', color);
        seg.style.filter = 'none';
      }
    });
    // Note: Percentage text is now updated in updateContextDisplay()
  }

  /**
   * Update context/token usage display
   * @param {Object} payload - Context info {current, max, usage, threshold}
   */
  function updateContextDisplay(payload) {
    const contextDisplay = document.getElementById('context-display');
    const contextText = document.getElementById('context-text');
    const contextPercent = document.getElementById('context-gauge-percent');

    if (!contextDisplay) {
      console.warn('Context display elements not found');
      return;
    }

    const { current, max, usage } = payload;

    // Update the gauge arc
    renderContextGauge(usage);

    // Update token counts (SVG text element)
    if (contextText) {
      const currentK = (current / 1000).toFixed(1);
      const maxK = (max / 1000).toFixed(0);
      contextText.textContent = `${currentK}k / ${maxK}k`;
    }

    // Update percentage with color coding (SVG text element)
    if (contextPercent) {
      contextPercent.textContent = `${Math.round(usage)}%`;
      // SVG uses class for styling, same as before
      contextPercent.classList.remove('warning', 'danger');
      if (usage > 90) {
        contextPercent.classList.add('danger');
      } else if (usage > 75) {
        contextPercent.classList.add('warning');
      }
    }

    // Also update metricsState and telemetry panel
    metricsState.context_percent = usage;
    updateTelemetryPanel();

    if (debugMode) {
      console.log(`Context update: ${current}/${max} tokens (${usage.toFixed(1)}%)`);
    }
  }

  /**
   * Check if text contains command tags
   */
  function containsCommandTags(text) {
    return text.includes('<command>') || text.includes('[Tool Result:');
  }

  /**
   * Check if text is ONLY debug content (no user-facing text)
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
   */
  function extractUserFacingText(text) {
    return text
      .replace(/<command>[\s\S]*?<\/command>/g, '')
      .replace(/\[Tool Result:[\s\S]*?\]/g, '')
      .trim();
  }

  /**
   * Extract command/debug portions from a message
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

  /**
   * Format command text for debug display
   */
  function formatCommandText(text) {
    // Highlight <command> tags in cyan, tool results in green
    return text
      .replace(/(<command>)/g, '<span style="color:#22d3ee">$1</span>')
      .replace(/(<\/command>)/g, '<span style="color:#22d3ee">$1</span>')
      .replace(/(\[Tool Result:[^\]]*\])/g, '<span style="color:#22c55e">$1</span>');
  }

  /**
   * Add a debug entry to the transcript
   */
  function addDebugEntry(label, content) {
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
      <div class="role">${escapeHtml(label)}</div>
      <div class="text">${formatCommandText(escapeHtml(content))}</div>
    `;
    // Visibility controlled by CSS via body.debug-mode class
    elements.transcript.appendChild(entry);
  }

  /**
   * Add a normal entry to the transcript
   */
  function addNormalEntry(role, text) {
    const entry = document.createElement('div');
    entry.className = `transcript-entry ${role}`;
    entry.innerHTML = `
      <div class="role">${escapeHtml(role)}</div>
      <div class="text">${formatMarkdown(text)}</div>
    `;
    elements.transcript.appendChild(entry);
  }

  function addTranscriptEntry(role, text) {
    // Remove placeholder if present
    const placeholder = elements.transcript.querySelector('.transcript-placeholder');
    if (placeholder) {
      placeholder.remove();
    }

    // Route tool role messages to debug entries (server sends role:"tool" for tool results)
    if (role === 'tool') {
      addDebugEntry('tool result', text);
      elements.transcript.scrollTop = elements.transcript.scrollHeight;
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
            elements.transcript.scrollTop = elements.transcript.scrollHeight;
            return;
          }
          if (firstItem.type === 'tool_result') {
            // Format tool results nicely
            parsed.forEach(item => {
              const formatted = `[Tool Result: ${item.tool_use_id}]\n${item.content}`;
              addDebugEntry('tool result', formatted);
            });
            elements.transcript.scrollTop = elements.transcript.scrollHeight;
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
      elements.transcript.scrollTop = elements.transcript.scrollHeight;
      return;
    }
    if (text.startsWith('[Tool Call:')) {
      addDebugEntry('tool call', text);
      elements.transcript.scrollTop = elements.transcript.scrollHeight;
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

    elements.transcript.scrollTop = elements.transcript.scrollHeight;
  }

  function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
  }

  /**
   * Format text with full markdown support using marked.js
   * Sanitized with DOMPurify for XSS protection
   */
  function formatMarkdown(text) {
    // Parse markdown to HTML, then sanitize
    const html = marked.parse(text, { breaks: true, gfm: true });
    return DOMPurify.sanitize(html);
  }

  // =============================================================================
  // LLM Streaming Handlers (ChatGPT-style real-time text)
  // =============================================================================

  /**
   * Handle stream_start: Create new assistant entry for streaming
   */
  function handleStreamStart(payload) {
    console.log('Stream start:', payload);

    // Update status to show responding
    updateState('speaking', 'Responding');

    // Cancel any existing stream
    if (streamingState.active) {
      console.warn('Stream start received while already streaming');
      finalizeStream();
    }

    // Remove placeholder if present
    const placeholder = elements.transcript.querySelector('.transcript-placeholder');
    if (placeholder) {
      placeholder.remove();
    }

    // Create new streaming entry
    const entry = document.createElement('div');
    entry.className = 'transcript-entry assistant streaming';
    entry.innerHTML = `
      <div class="role">assistant</div>
      <div class="text"></div>
    `;
    elements.transcript.appendChild(entry);

    // Update streaming state
    streamingState.active = true;
    streamingState.streamId = payload.stream_id;
    streamingState.entryElement = entry;
    streamingState.textElement = entry.querySelector('.text');
    streamingState.content = '';

    // Start hesitation ring animation for token timing visualization
    startHesitationAnimation();

    elements.transcript.scrollTop = elements.transcript.scrollHeight;
  }

  /**
   * Handle stream_delta: Append text to current streaming entry
   * Uses debounced markdown rendering (max 10Hz) to avoid heavyweight parsing per token
   */
  function handleStreamDelta(payload) {
    // Ignore deltas for different stream IDs
    if (!streamingState.active || payload.stream_id !== streamingState.streamId) {
      console.warn('Ignoring stale stream delta:', payload.stream_id, 'expected:', streamingState.streamId);
      return;
    }

    // Track token timing for hesitation ring
    onTokenEvent();

    // Emit token event for decoupled visualization modules
    if (typeof DawnEvents !== 'undefined') {
      DawnEvents.emit('token', { timestamp: performance.now() });
    }

    // Append delta to content
    streamingState.content += payload.delta;

    // Debounced markdown rendering - max 10Hz to avoid parsing entire content per token
    const nowMs = performance.now();
    if (nowMs - streamingState.lastRenderMs >= streamingState.renderDebounceMs) {
      // Enough time has passed - render immediately
      streamingState.textElement.innerHTML = formatMarkdown(streamingState.content);
      streamingState.lastRenderMs = nowMs;
      streamingState.pendingRender = false;
    } else if (!streamingState.pendingRender) {
      // Schedule a render for when debounce period ends
      streamingState.pendingRender = true;
      const delay = streamingState.renderDebounceMs - (nowMs - streamingState.lastRenderMs);
      setTimeout(() => {
        if (streamingState.active && streamingState.pendingRender) {
          streamingState.textElement.innerHTML = formatMarkdown(streamingState.content)
            .replace(/\n/g, '<br>');
          streamingState.lastRenderMs = performance.now();
          streamingState.pendingRender = false;
          elements.transcript.scrollTop = elements.transcript.scrollHeight;
        }
      }, delay);
    }

    elements.transcript.scrollTop = elements.transcript.scrollHeight;
  }

  /**
   * Handle stream_end: Finalize streaming entry
   */
  function handleStreamEnd(payload) {
    console.log('Stream end:', payload);

    // Ignore end for different stream IDs
    if (payload.stream_id !== streamingState.streamId) {
      console.warn('Ignoring stale stream end:', payload.stream_id, 'expected:', streamingState.streamId);
      return;
    }

    finalizeStream();
  }

  /**
   * Finalize the current streaming entry
   */
  function finalizeStream() {
    if (!streamingState.active) {
      return;
    }

    // Update status back to idle
    updateState('idle', null);

    // Stop hesitation ring animation
    stopHesitationAnimation();

    // Final render to ensure all content is displayed (in case debounce was pending)
    if (streamingState.textElement && streamingState.content) {
      streamingState.textElement.innerHTML = formatMarkdown(streamingState.content);
    }

    // Remove streaming class
    if (streamingState.entryElement) {
      streamingState.entryElement.classList.remove('streaming');
    }

    // Reset state
    streamingState.active = false;
    streamingState.streamId = null;
    streamingState.entryElement = null;
    streamingState.textElement = null;
    streamingState.content = '';
    streamingState.pendingRender = false;
  }

  // =============================================================================
  // Metrics Handling (for multi-ring visualization)
  // =============================================================================

  /**
   * Handle metrics_update message from server
   * Updates global metricsState and drives multi-ring visualization + telemetry panel
   */
  function handleMetricsUpdate(payload) {
    metricsState.state = payload.state || 'idle';
    metricsState.ttft_ms = payload.ttft_ms || 0;
    metricsState.token_rate = payload.token_rate || 0;
    // Only update context_percent if value is valid (>= 0), otherwise keep previous
    if (payload.context_percent >= 0) {
      metricsState.context_percent = payload.context_percent;
    }
    metricsState.lastUpdate = performance.now();

    // Preserve last non-zero values for display after streaming ends
    // Also track session averages
    if (metricsState.ttft_ms > 0) {
      metricsState.last_ttft_ms = metricsState.ttft_ms;
      avgState.ttft.sum += metricsState.ttft_ms;
      avgState.ttft.count++;
    }
    if (metricsState.token_rate > 0) {
      metricsState.last_token_rate = metricsState.token_rate;
      avgState.tokenRate.sum += metricsState.token_rate;
      avgState.tokenRate.count++;
    }

    // Update throughput ring based on token rate
    // Normalize: 0 tokens/sec = 0%, 80 tokens/sec = 100%
    const throughputFill = Math.min(metricsState.token_rate / 80, 1.0);
    renderSegmentedArc(elements.ringThroughput, THROUGHPUT_RADIUS, THROUGHPUT_SEGMENTS, throughputFill);

    // Update telemetry panel (right side discrete readouts)
    updateTelemetryPanel();

    // Debug log (only when debug mode is enabled)
    if (debugMode) {
      console.log('Metrics update:', metricsState);
    }
  }

  /**
   * Apply color class to metric value based on thresholds
   * @param {Element} el - DOM element
   * @param {number} value - Current value
   * @param {number} normalMax - Max value for normal (cyan)
   * @param {number} elevatedMax - Max value for elevated (amber), above = extreme (red)
   */
  function applyMetricColor(el, value, normalMax, elevatedMax) {
    el.classList.remove('metric-normal', 'metric-elevated', 'metric-extreme');
    if (value <= 0) return;  // No color for placeholder values

    if (value <= normalMax) {
      el.classList.add('metric-normal');
    } else if (value <= elevatedMax) {
      el.classList.add('metric-elevated');
    } else {
      el.classList.add('metric-extreme');
    }
  }

  /**
   * Update the telemetry panel with current metrics
   * Per design: discrete numeric readouts, visually quieter than rings
   * Color coding: cyan (normal) -> amber (elevated) -> red (extreme)
   */
  function updateTelemetryPanel() {
    const panel = document.getElementById('telemetry-panel');
    const tokenRate = document.getElementById('telem-token-rate');
    const tokenRateAvg = document.getElementById('telem-token-rate-avg');
    const ttft = document.getElementById('telem-ttft');
    const ttftAvg = document.getElementById('telem-ttft-avg');
    const latency = document.getElementById('telem-latency');
    const latencyAvg = document.getElementById('telem-latency-avg');

    if (!panel) return;

    // Update token rate with color coding
    // Normal: >30 tok/s, Elevated: 15-30, Extreme: <15
    if (tokenRate) {
      const rate = metricsState.token_rate || metricsState.last_token_rate;
      tokenRate.innerHTML = rate > 0 ? `${rate.toFixed(1)} <small>tok/s</small>` : '-- <small>tok/s</small>';
      // Invert thresholds - higher is better for token rate
      tokenRate.classList.remove('metric-normal', 'metric-elevated', 'metric-extreme');
      if (rate > 0) {
        if (rate >= 30) {
          tokenRate.classList.add('metric-normal');
        } else if (rate >= 15) {
          tokenRate.classList.add('metric-elevated');
        } else {
          tokenRate.classList.add('metric-extreme');
        }
      }
    }
    // Update token rate average
    if (tokenRateAvg && avgState.tokenRate.count > 0) {
      const avg = avgState.tokenRate.sum / avgState.tokenRate.count;
      tokenRateAvg.textContent = `avg ${avg.toFixed(1)}`;
    }

    // Update TTFT with color coding
    // Normal: <500ms, Elevated: 500-1500ms, Extreme: >1500ms
    if (ttft) {
      const ms = metricsState.ttft_ms || metricsState.last_ttft_ms;
      ttft.innerHTML = ms > 0 ? `${ms} <small>ms</small>` : '-- <small>ms</small>';
      applyMetricColor(ttft, ms, 500, 1500);
    }
    // Update TTFT average
    if (ttftAvg && avgState.ttft.count > 0) {
      const avg = avgState.ttft.sum / avgState.ttft.count;
      ttftAvg.textContent = `avg ${Math.round(avg)}`;
    }

    // Update latency variance with color coding
    // Show as milliseconds variance (stddev of inter-token intervals)
    // Normal: <10ms, Elevated: 10-25ms, Extreme: >25ms
    if (latency) {
      const load = hesitationState.loadSmooth;
      if (load > 0.005) {
        // load 0-1 maps to ~3-40ms stddev range
        const msVar = Math.round(3 + load * 37);
        latency.innerHTML = `${msVar} <small>ms var</small>`;
        applyMetricColor(latency, msVar, 10, 25);
        // Track latency average
        avgState.latency.sum += msVar;
        avgState.latency.count++;
      } else {
        latency.innerHTML = '-- <small>ms var</small>';
        latency.classList.remove('metric-normal', 'metric-elevated', 'metric-extreme');
      }
    }
    // Update latency average
    if (latencyAvg && avgState.latency.count > 0) {
      const avg = avgState.latency.sum / avgState.latency.count;
      latencyAvg.textContent = `avg ${Math.round(avg)}`;
    }

    // Dim ring container when idle (state == 'idle') - not the telemetry panel
    if (elements.ringContainer) {
      if (metricsState.state === 'idle') {
        elements.ringContainer.classList.add('idle');
      } else {
        elements.ringContainer.classList.remove('idle');
      }
    }
  }

  // =============================================================================
  // Event Handlers
  // =============================================================================
  function handleSend() {
    const text = elements.textInput.value.trim();
    if (text) {
      if (sendTextMessage(text)) {
        elements.textInput.value = '';
      }
    }
  }

  function handleKeydown(event) {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault();
      handleSend();
    }
  }

  // =============================================================================
  // Opus Worker Initialization
  // =============================================================================

  /**
   * Initialize the Opus Web Worker for audio encoding/decoding
   */
  function initOpusWorker() {
    try {
      opusWorker = new Worker('js/opus-worker.js');

      opusWorker.onmessage = function(e) {
        const msg = e.data;

        switch (msg.type) {
          case 'ready':
            console.log('Opus worker: WASM loaded');
            // Initialize encoder/decoder
            opusWorker.postMessage({ type: 'init' });
            break;

          case 'init_done':
            if (msg.webcodecs) {
              console.log('Opus worker: WebCodecs initialized successfully');
              opusReady = true;
              // If already connected, send capability update to enable Opus
              if (ws && ws.readyState === WebSocket.OPEN) {
                console.log('Sending Opus capability update');
                ws.send(JSON.stringify({
                  type: 'capabilities_update',
                  payload: {
                    capabilities: { audio_codecs: ['opus', 'pcm'] }
                  }
                }));
              }
            } else {
              console.log('Opus worker: WebCodecs not available, using PCM only');
              opusReady = false;
            }
            break;

          case 'encoded':
            // Send encoded Opus data to server
            if (msg.data && msg.data.length > 0) {
              sendOpusData(msg.data);
            }
            break;

          case 'decoded':
            // Queue decoded PCM for playback
            if (msg.data && msg.data.length > 0) {
              queueDecodedAudio(msg.data);
            }
            break;

          case 'error':
            console.error('Opus worker error:', msg.error);
            break;
        }
      };

      opusWorker.onerror = function(e) {
        console.error('Opus worker failed:', e.message);
        opusWorker = null;
        opusReady = false;
      };

    } catch (e) {
      console.warn('Failed to create Opus worker:', e);
      opusWorker = null;
      opusReady = false;
    }
  }

  /**
   * Send encoded Opus data to server
   */
  function sendOpusData(opusData) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    // Create message: [type byte][Opus data]
    const payload = new Uint8Array(1 + opusData.length);
    payload[0] = WS_BIN_AUDIO_IN;
    payload.set(opusData, 1);

    ws.send(payload.buffer);
  }

  /**
   * Queue decoded audio samples for playback
   */
  function queueDecodedAudio(pcmData) {
    // Convert Int16 to Uint8 for existing playback pipeline
    const bytes = new Uint8Array(pcmData.buffer, pcmData.byteOffset, pcmData.byteLength);
    audioChunks.push(bytes);

    // If we were waiting for decode to complete, trigger playback now
    if (pendingDecodePlayback) {
      pendingDecodePlayback = false;
      if (audioChunks.length > 0) {
        playAccumulatedAudio();
      }
    }
  }

  // =============================================================================
  // Initialization
  // =============================================================================
  async function init() {
    // Initialize Opus worker first (before connect)
    initOpusWorker();

    // Event listeners
    elements.sendBtn.addEventListener('click', handleSend);
    elements.textInput.addEventListener('keydown', handleKeydown);

    // Mic button - push to talk
    if (elements.micBtn) {
      elements.micBtn.addEventListener('mousedown', function(e) {
        e.preventDefault();
        if (!isRecording && audioSupported) {
          startRecording();
        }
      });

      elements.micBtn.addEventListener('mouseup', function(e) {
        e.preventDefault();
        if (isRecording) {
          stopRecording();
        }
      });

      elements.micBtn.addEventListener('mouseleave', function(e) {
        // Stop recording if mouse leaves button while pressed
        if (isRecording) {
          stopRecording();
        }
      });

      // Touch events for mobile
      elements.micBtn.addEventListener('touchstart', function(e) {
        e.preventDefault();
        if (!isRecording && audioSupported) {
          startRecording();
        }
      });

      elements.micBtn.addEventListener('touchend', function(e) {
        e.preventDefault();
        if (isRecording) {
          stopRecording();
        }
      });
    }

    // Debug mode toggle
    elements.debugBtn.addEventListener('click', function() {
      debugMode = !debugMode;
      this.classList.toggle('active', debugMode);
      // Toggle body class for CSS-based visibility control
      document.body.classList.toggle('debug-mode', debugMode);
      console.log('Debug mode:', debugMode ? 'enabled' : 'disabled');

      // Request system prompt when debug is enabled
      if (debugMode && ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'get_system_prompt' }));
      }

      // Scroll to bottom to see newly visible entries
      elements.transcript.scrollTop = elements.transcript.scrollHeight;
    });

    // Initialize audio support
    const audioResult = await initAudio();
    if (audioResult.supported && elements.micBtn) {
      elements.micBtn.disabled = false;
      elements.micBtn.title = 'Hold to speak';
    } else if (elements.micBtn) {
      // Show why audio is disabled
      elements.micBtn.title = audioResult.reason || 'Audio not available';
      console.warn('Mic disabled:', audioResult.reason);
    }

    // Draw default waveform shape (circle) on page load
    drawDefaultWaveform();

    // Initialize multi-ring structure (throughput + hesitation rings)
    initializeRings();

    // Initialize context pressure gauge
    initializeContextGauge();

    // Initialize settings panel
    initSettingsElements();
    initSettingsListeners();
    initToolsSection();

    // Connect to WebSocket
    connect();

    // Reconnect on visibility change
    document.addEventListener('visibilitychange', function() {
      if (!document.hidden && (!ws || ws.readyState !== WebSocket.OPEN)) {
        connect();
      }
    });

    // Fetch and display version info in footer
    fetchVersionInfo();

    console.log('DAWN WebUI initialized (audio:', audioResult.supported ? 'enabled' : 'disabled', ')');
  }

  /**
   * Fetch version info from /health endpoint and update footer
   */
  async function fetchVersionInfo() {
    try {
      const response = await fetch('/health');
      if (response.ok) {
        const data = await response.json();
        const footerVersion = document.getElementById('footer-version');
        if (footerVersion && data.version && data.git_sha) {
          footerVersion.textContent = `Dawn WebUI v${data.version}: ${data.git_sha}`;
        }
      }
    } catch (e) {
      console.warn('Failed to fetch version info:', e);
    }
  }

  // =============================================================================
  // Settings Panel
  // =============================================================================

  // Settings state
  let currentConfig = null;
  let currentSecrets = null;
  let restartRequiredFields = [];
  let changedFields = new Set();
  let dynamicOptions = { asr_models: [], tts_voices: [], bind_addresses: [] };

  // Settings DOM elements (added after init)
  const settingsElements = {};

  // Settings schema for dynamic form generation
  const SETTINGS_SCHEMA = {
    general: {
      label: 'General',
      icon: '&#x2699;',
      fields: {
        ai_name: { type: 'text', label: 'AI Name / Wake Word', hint: 'Wake word to activate voice input' },
        log_file: { type: 'text', label: 'Log File Path', placeholder: 'Leave empty for stdout', hint: 'Path to log file, or empty for console output' }
      }
    },
    persona: {
      label: 'Persona',
      icon: '&#x1F464;',
      fields: {
        description: { type: 'textarea', label: 'AI Description', rows: 3, placeholder: 'Custom personality description', hint: 'Personality and behavior instructions for the AI. Changes apply to new conversations only.' }
      }
    },
    localization: {
      label: 'Localization',
      icon: '&#x1F30D;',
      fields: {
        location: { type: 'text', label: 'Location', placeholder: 'e.g., San Francisco, CA', hint: 'Default location for weather and local queries' },
        timezone: {
          type: 'select',
          label: 'Timezone',
          hint: 'IANA timezone for time-related responses',
          options: [
            '', // System default
            'America/New_York',
            'America/Chicago',
            'America/Denver',
            'America/Phoenix',
            'America/Los_Angeles',
            'America/Anchorage',
            'America/Honolulu',
            'America/Toronto',
            'America/Vancouver',
            'America/Mexico_City',
            'America/Sao_Paulo',
            'America/Buenos_Aires',
            'Europe/London',
            'Europe/Paris',
            'Europe/Berlin',
            'Europe/Rome',
            'Europe/Madrid',
            'Europe/Amsterdam',
            'Europe/Moscow',
            'Asia/Tokyo',
            'Asia/Shanghai',
            'Asia/Hong_Kong',
            'Asia/Singapore',
            'Asia/Seoul',
            'Asia/Mumbai',
            'Asia/Dubai',
            'Asia/Bangkok',
            'Australia/Sydney',
            'Australia/Melbourne',
            'Australia/Perth',
            'Pacific/Auckland',
            'Pacific/Fiji',
            'UTC'
          ]
        },
        units: { type: 'select', label: 'Units', options: ['imperial', 'metric'], hint: 'Measurement system for weather and calculations' }
      }
    },
    audio: {
      label: 'Audio',
      icon: '&#x1F50A;',
      fields: {
        backend: { type: 'select', label: 'Backend', options: ['auto', 'pulse', 'alsa'], restart: true, hint: 'Audio system: auto detects PulseAudio or falls back to ALSA' },
        capture_device: { type: 'text', label: 'Capture Device', restart: true, hint: 'Microphone device name (e.g., default, hw:0,0)' },
        playback_device: { type: 'text', label: 'Playback Device', restart: true, hint: 'Speaker device name (e.g., default, hw:0,0)' },
        bargein: {
          type: 'group',
          label: 'Barge-In',
          fields: {
            enabled: { type: 'checkbox', label: 'Enable Barge-In', hint: 'Allow interrupting AI speech with new voice input' },
            cooldown_ms: { type: 'number', label: 'Cooldown (ms)', min: 0, hint: 'Minimum time between barge-in events' },
            startup_cooldown_ms: { type: 'number', label: 'Startup Cooldown (ms)', min: 0, hint: 'Block barge-in when TTS first starts speaking' }
          }
        }
      }
    },
    commands: {
      label: 'Commands',
      icon: '&#x2328;',
      fields: {
        processing_mode: { type: 'select', label: 'Processing Mode', options: ['direct_only', 'llm_only', 'direct_first'], restart: true, hint: 'direct_only: pattern matching only, llm_only: AI interprets all commands, direct_first: try patterns then AI' }
      }
    },
    vad: {
      label: 'Voice Activity Detection',
      icon: '&#x1F3A4;',
      fields: {
        speech_threshold: { type: 'number', label: 'Speech Threshold', min: 0, max: 1, step: 0.05, hint: 'VAD confidence to start listening (higher = less sensitive)' },
        speech_threshold_tts: { type: 'number', label: 'Speech Threshold (TTS)', min: 0, max: 1, step: 0.05, hint: 'VAD threshold during TTS playback (higher to ignore echo)' },
        silence_threshold: { type: 'number', label: 'Silence Threshold', min: 0, max: 1, step: 0.05, hint: 'VAD confidence to detect end of speech' },
        end_of_speech_duration: { type: 'number', label: 'End of Speech (sec)', min: 0, step: 0.1, hint: 'Silence duration before processing speech' },
        max_recording_duration: { type: 'number', label: 'Max Recording (sec)', min: 1, step: 1, hint: 'Maximum single utterance length' },
        preroll_ms: { type: 'number', label: 'Preroll (ms)', min: 0, hint: 'Audio captured before VAD trigger (catches word beginnings)' }
      }
    },
    asr: {
      label: 'Speech Recognition',
      icon: '&#x1F4DD;',
      fields: {
        models_path: { type: 'text', label: 'Models Path', restart: true, hint: 'Directory containing Whisper model files' },
        model: { type: 'dynamic_select', label: 'Model', restart: true, hint: 'Whisper model size', dynamicKey: 'asr_models', allowCustom: true }
      }
    },
    tts: {
      label: 'Text-to-Speech',
      icon: '&#x1F5E3;',
      fields: {
        models_path: { type: 'text', label: 'Models Path', restart: true, hint: 'Directory containing Piper voice model files' },
        voice_model: { type: 'dynamic_select', label: 'Voice Model', restart: true, hint: 'Piper voice model', dynamicKey: 'tts_voices', allowCustom: true },
        length_scale: { type: 'number', label: 'Speed (0.5-2.0)', min: 0.5, max: 2.0, step: 0.05, hint: 'Speaking rate: <1.0 = faster, >1.0 = slower' }
      }
    },
    llm: {
      label: 'Language Model',
      icon: '&#x1F916;',
      fields: {
        type: { type: 'select', label: 'Type', options: ['cloud', 'local'], hint: 'Use cloud APIs or local llama-server' },
        max_tokens: { type: 'number', label: 'Max Tokens', min: 100, hint: 'Maximum tokens in LLM response' },
        cloud: {
          type: 'group',
          label: 'Cloud Settings',
          fields: {
            provider: { type: 'select', label: 'Provider', options: ['openai', 'claude'], hint: 'Cloud LLM provider' },
            openai_model: { type: 'text', label: 'OpenAI Model', hint: 'e.g., gpt-4o, gpt-4-turbo, gpt-4o-mini' },
            claude_model: { type: 'text', label: 'Claude Model', hint: 'e.g., claude-sonnet-4-20250514, claude-opus-4-20250514' },
            endpoint: { type: 'text', label: 'Custom Endpoint', placeholder: 'Leave empty for default', hint: 'Override API endpoint (for proxies or compatible APIs)' },
            vision_enabled: { type: 'checkbox', label: 'Enable Vision', hint: 'Allow image analysis with vision-capable models' }
          }
        },
        local: {
          type: 'group',
          label: 'Local Settings',
          fields: {
            endpoint: { type: 'text', label: 'Endpoint', hint: 'llama-server URL (e.g., http://127.0.0.1:8080)' },
            model: { type: 'text', label: 'Model', placeholder: 'Leave empty for server default', hint: 'Model name if server hosts multiple models' },
            vision_enabled: { type: 'checkbox', label: 'Enable Vision', hint: 'Enable for multimodal models like LLaVA' }
          }
        },
        tools: {
          type: 'group',
          label: 'Tool Calling',
          fields: {
            native_enabled: { type: 'checkbox', label: 'Enable Native Tools', restart: true, hint: 'Use native function/tool calling instead of <command> tags (requires compatible LLM)' }
          }
        }
      }
    },
    search: {
      label: 'Web Search',
      icon: '&#x1F50D;',
      fields: {
        engine: { type: 'select', label: 'Engine', options: ['searxng', 'disabled'], hint: 'Search engine for web queries (SearXNG is privacy-focused)' },
        endpoint: { type: 'text', label: 'Endpoint', hint: 'SearXNG instance URL (e.g., http://localhost:8888)' },
        summarizer: {
          type: 'group',
          label: 'Result Summarizer',
          fields: {
            backend: { type: 'select', label: 'Backend', options: ['disabled', 'local', 'default'], hint: 'disabled: no summarization, local: use local LLM, default: use active LLM' },
            threshold_bytes: { type: 'number', label: 'Threshold (bytes)', min: 0, step: 512, hint: 'Summarize results larger than this (0 = always summarize)' },
            target_words: { type: 'number', label: 'Target Words', min: 50, step: 50, hint: 'Target word count for summarized output' }
          }
        }
      }
    },
    url_fetcher: {
      label: 'URL Fetcher',
      icon: '&#x1F310;',
      fields: {
        flaresolverr: {
          type: 'group',
          label: 'FlareSolverr',
          fields: {
            enabled: { type: 'checkbox', label: 'Enable FlareSolverr', hint: 'Auto-fallback for sites with Cloudflare protection (requires FlareSolverr service)' },
            endpoint: { type: 'text', label: 'Endpoint', hint: 'FlareSolverr API URL (e.g., http://localhost:8191/v1)' },
            timeout_sec: { type: 'number', label: 'Timeout (sec)', min: 1, max: 120, hint: 'Request timeout for FlareSolverr' },
            max_response_bytes: { type: 'number', label: 'Max Response (bytes)', min: 1024, step: 1024, hint: 'Maximum response size to accept' }
          }
        }
      }
    },
    mqtt: {
      label: 'MQTT',
      icon: '&#x1F4E1;',
      fields: {
        enabled: { type: 'checkbox', label: 'Enable MQTT', hint: 'Connect to MQTT broker for smart home control' },
        broker: { type: 'text', label: 'Broker Address', hint: 'MQTT broker hostname or IP address' },
        port: { type: 'number', label: 'Port', min: 1, max: 65535, hint: 'MQTT broker port (default: 1883)' }
      }
    },
    network: {
      label: 'Network (DAP)',
      description: 'Dawn Audio Protocol server for ESP32 and other remote voice clients',
      icon: '&#x1F4F6;',
      fields: {
        enabled: { type: 'checkbox', label: 'Enable DAP Server', restart: true, hint: 'Accept connections from remote voice clients' },
        host: { type: 'dynamic_select', label: 'Bind Address', restart: true, hint: 'Network interface to listen on', dynamicKey: 'bind_addresses' },
        port: { type: 'number', label: 'Port', min: 1, max: 65535, restart: true, hint: 'TCP port for DAP connections (default: 5000)' },
        workers: { type: 'number', label: 'Workers', min: 1, max: 8, restart: true, hint: 'Concurrent client processing threads' }
      }
    },
    webui: {
      label: 'WebUI',
      icon: '&#x1F310;',
      fields: {
        enabled: { type: 'checkbox', label: 'Enable WebUI', hint: 'Browser-based interface for voice interaction' },
        port: { type: 'number', label: 'Port', min: 1, max: 65535, restart: true, hint: 'HTTP/WebSocket port (default: 3000)' },
        max_clients: { type: 'number', label: 'Max Clients', min: 1, restart: true, hint: 'Maximum concurrent browser connections' },
        workers: { type: 'number', label: 'ASR Workers', min: 1, max: 8, restart: true, hint: 'Parallel speech recognition threads' },
        bind_address: { type: 'dynamic_select', label: 'Bind Address', restart: true, hint: 'Network interface to listen on', dynamicKey: 'bind_addresses' },
        https: { type: 'checkbox', label: 'Enable HTTPS', restart: true, hint: 'Required for microphone access on remote connections' }
      }
    },
    shutdown: {
      label: 'Shutdown',
      icon: '&#x1F512;',
      fields: {
        enabled: { type: 'checkbox', label: 'Enable Voice Shutdown', hint: 'Allow system shutdown via voice command (disabled by default for security)' },
        passphrase: { type: 'text', label: 'Passphrase', hint: 'Secret phrase required to authorize shutdown (leave empty for no passphrase)' }
      }
    },
    debug: {
      label: 'Debug',
      icon: '&#x1F41B;',
      fields: {
        mic_record: { type: 'checkbox', label: 'Record Microphone' },
        asr_record: { type: 'checkbox', label: 'Record ASR Input' },
        aec_record: { type: 'checkbox', label: 'Record AEC' },
        record_path: { type: 'text', label: 'Recording Path' }
      }
    },
    tui: {
      label: 'Terminal UI',
      icon: '&#x1F5A5;',
      fields: {
        enabled: { type: 'checkbox', label: 'Enable TUI', restart: true, hint: 'Show terminal dashboard with real-time metrics' }
      }
    },
    paths: {
      label: 'Paths',
      icon: '&#x1F4C1;',
      fields: {
        music_dir: { type: 'text', label: 'Music Directory', hint: 'Path to music library for playback commands' },
        commands_config: { type: 'text', label: 'Commands Config', restart: true, hint: 'Path to device/command mappings JSON file' }
      }
    }
  };

  /**
   * Initialize settings panel elements
   */
  function initSettingsElements() {
    settingsElements.panel = document.getElementById('settings-panel');
    settingsElements.overlay = document.getElementById('settings-overlay');
    settingsElements.closeBtn = document.getElementById('settings-close');
    settingsElements.openBtn = document.getElementById('settings-btn');
    settingsElements.configPath = document.getElementById('config-path-display');
    settingsElements.secretsPath = document.getElementById('secrets-path-display');
    settingsElements.sectionsContainer = document.getElementById('settings-sections');
    settingsElements.saveConfigBtn = document.getElementById('save-config-btn');
    settingsElements.saveSecretsBtn = document.getElementById('save-secrets-btn');
    settingsElements.resetBtn = document.getElementById('reset-config-btn');
    settingsElements.restartNotice = document.getElementById('restart-notice');

    // Secret inputs
    settingsElements.secretOpenai = document.getElementById('secret-openai');
    settingsElements.secretClaude = document.getElementById('secret-claude');
    settingsElements.secretMqttUser = document.getElementById('secret-mqtt-user');
    settingsElements.secretMqttPass = document.getElementById('secret-mqtt-pass');

    // Secret status indicators
    settingsElements.statusOpenai = document.getElementById('status-openai');
    settingsElements.statusClaude = document.getElementById('status-claude');
    settingsElements.statusMqttUser = document.getElementById('status-mqtt-user');
    settingsElements.statusMqttPass = document.getElementById('status-mqtt-pass');

    // SmartThings elements
    settingsElements.stStatusIndicator = document.getElementById('st-status-indicator');
    settingsElements.stStatusText = settingsElements.stStatusIndicator?.querySelector('.st-status-text');
    settingsElements.stDevicesCountRow = document.getElementById('st-devices-count-row');
    settingsElements.stDevicesCount = document.getElementById('st-devices-count');
    settingsElements.stConnectBtn = document.getElementById('st-connect-btn');
    settingsElements.stRefreshBtn = document.getElementById('st-refresh-btn');
    settingsElements.stDisconnectBtn = document.getElementById('st-disconnect-btn');
    settingsElements.stDevicesList = document.getElementById('st-devices-list');
    settingsElements.stDevicesContainer = document.getElementById('st-devices-container');
    settingsElements.stNotConfigured = document.getElementById('st-not-configured');
  }

  /**
   * Open settings panel and request config
   */
  function openSettings() {
    if (!settingsElements.panel) return;

    settingsElements.panel.classList.remove('hidden');
    settingsElements.overlay.classList.remove('hidden');

    // Request config, models, and interfaces from server
    requestConfig();
    requestModelsList();
    requestInterfacesList();
    requestSmartThingsStatus();
  }

  /**
   * Close settings panel
   */
  function closeSettings() {
    if (!settingsElements.panel) return;

    settingsElements.panel.classList.add('hidden');
    settingsElements.overlay.classList.add('hidden');
  }

  /**
   * Request current configuration from server
   */
  function requestConfig() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      return;
    }

    ws.send(JSON.stringify({ type: 'get_config' }));
    console.log('Requested configuration from server');
  }

  /**
   * Request available ASR/TTS models list from server
   */
  function requestModelsList() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    ws.send(JSON.stringify({ type: 'list_models' }));
    console.log('Requested models list from server');
  }

  /**
   * Handle models list response from server
   */
  function handleModelsListResponse(payload) {
    if (payload.asr_models) {
      dynamicOptions.asr_models = payload.asr_models;
    }
    if (payload.tts_voices) {
      dynamicOptions.tts_voices = payload.tts_voices;
    }
    console.log('Received models list:', dynamicOptions);

    // Update any already-rendered dynamic selects
    updateDynamicSelects();
  }

  /**
   * Request available network interfaces from server
   */
  function requestInterfacesList() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    ws.send(JSON.stringify({ type: 'list_interfaces' }));
    console.log('Requested interfaces list from server');
  }

  /**
   * Handle interfaces list response from server
   */
  function handleInterfacesListResponse(payload) {
    if (payload.addresses) {
      dynamicOptions.bind_addresses = payload.addresses;
    }
    console.log('Received interfaces list:', dynamicOptions.bind_addresses);

    // Update any already-rendered dynamic selects
    updateDynamicSelects();
  }

  /**
   * Update dynamic select dropdowns with current options
   */
  function updateDynamicSelects() {
    document.querySelectorAll('select[data-dynamic-key]').forEach(select => {
      const key = select.dataset.dynamicKey;
      const options = dynamicOptions[key] || [];
      const currentValue = select.value;

      // Clear existing options except the first (current value placeholder)
      while (select.options.length > 1) {
        select.remove(1);
      }

      // Add options from server (skip current value to avoid duplicate)
      options.forEach(opt => {
        if (opt === currentValue) return; // Skip - already in first option
        const option = document.createElement('option');
        option.value = opt;
        option.textContent = opt;
        select.appendChild(option);
      });

      // If current value isn't in options, mark it as "(current)"
      if (currentValue && !options.includes(currentValue)) {
        select.options[0].textContent = currentValue + ' (current)';
      }
    });
  }

  /**
   * Handle config response from server
   */
  function handleGetConfigResponse(payload) {
    console.log('Received config:', payload);

    currentConfig = payload.config;
    currentSecrets = payload.secrets;
    restartRequiredFields = payload.requires_restart || [];
    changedFields.clear();

    // Update path displays
    if (settingsElements.configPath) {
      settingsElements.configPath.textContent = payload.config_path || 'Unknown';
    }
    if (settingsElements.secretsPath) {
      settingsElements.secretsPath.textContent = payload.secrets_path || 'Unknown';
    }

    // Render settings sections
    renderSettingsSections();

    // Update secrets status
    updateSecretsStatus(currentSecrets);

    // Hide restart notice initially
    if (settingsElements.restartNotice) {
      settingsElements.restartNotice.classList.add('hidden');
    }

    // Initialize audio backend state (grey out or request devices)
    const backendSelect = document.getElementById('setting-audio-backend');
    if (backendSelect) {
      // Add backend change listener
      backendSelect.addEventListener('change', () => {
        updateAudioBackendState(backendSelect.value);
      });

      // Initialize state based on current value
      updateAudioBackendState(backendSelect.value);
    }

    // Update LLM runtime controls
    if (payload.llm_runtime) {
      updateLlmControls(payload.llm_runtime);
    }
  }

  /**
   * Render settings sections dynamically from schema
   */
  function renderSettingsSections() {
    if (!settingsElements.sectionsContainer || !currentConfig) return;

    settingsElements.sectionsContainer.innerHTML = '';

    for (const [sectionKey, sectionDef] of Object.entries(SETTINGS_SCHEMA)) {
      const configSection = currentConfig[sectionKey] || {};
      const sectionEl = createSettingsSection(sectionKey, sectionDef, configSection);
      settingsElements.sectionsContainer.appendChild(sectionEl);
    }
  }

  /**
   * Create a settings section element
   */
  function createSettingsSection(key, def, configData) {
    const section = document.createElement('div');
    section.className = 'settings-section';
    section.dataset.section = key;

    // Header
    const header = document.createElement('h3');
    header.className = 'section-header';
    header.innerHTML = `
      <span class="section-icon">${def.icon}</span>
      ${def.label}
      <span class="section-toggle">&#9660;</span>
    `;
    header.addEventListener('click', () => {
      header.classList.toggle('collapsed');
      content.classList.toggle('collapsed');
    });

    // Content
    const content = document.createElement('div');
    content.className = 'section-content';

    // Add section description if present
    if (def.description) {
      const desc = document.createElement('p');
      desc.className = 'section-description';
      desc.textContent = def.description;
      content.appendChild(desc);
    }

    // Render fields
    for (const [fieldKey, fieldDef] of Object.entries(def.fields)) {
      const value = configData[fieldKey];
      const fieldEl = createSettingField(key, fieldKey, fieldDef, value);
      content.appendChild(fieldEl);
    }

    section.appendChild(header);
    section.appendChild(content);

    return section;
  }

  /**
   * Create a setting field element
   */
  function createSettingField(sectionKey, fieldKey, def, value) {
    const fullKey = `${sectionKey}.${fieldKey}`;

    // Handle nested groups
    if (def.type === 'group') {
      const groupEl = document.createElement('div');
      groupEl.className = 'setting-group';
      groupEl.innerHTML = `<div class="group-label">${def.label}</div>`;

      for (const [subKey, subDef] of Object.entries(def.fields)) {
        const subValue = value ? value[subKey] : undefined;
        const fieldEl = createSettingField(`${sectionKey}.${fieldKey}`, subKey, subDef, subValue);
        groupEl.appendChild(fieldEl);
      }

      return groupEl;
    }

    const item = document.createElement('div');
    item.className = def.type === 'checkbox' ? 'setting-item setting-item-row' : 'setting-item';

    // Add tooltip hint if defined
    if (def.hint) {
      item.title = def.hint;
    }

    const needsRestart = def.restart || restartRequiredFields.includes(fullKey);
    const restartBadge = needsRestart ? '<span class="restart-badge">restart</span>' : '';

    let inputHtml = '';
    const inputId = `setting-${sectionKey}-${fieldKey}`.replace(/\./g, '-');

    switch (def.type) {
      case 'text':
        inputHtml = `<input type="text" id="${inputId}" value="${escapeAttr(value || '')}" placeholder="${def.placeholder || ''}" data-key="${fullKey}">`;
        break;
      case 'number':
        const numAttrs = [
          def.min !== undefined ? `min="${def.min}"` : '',
          def.max !== undefined ? `max="${def.max}"` : '',
          def.step !== undefined ? `step="${def.step}"` : ''
        ].filter(Boolean).join(' ');
        inputHtml = `<input type="number" id="${inputId}" value="${formatNumber(value)}" ${numAttrs} data-key="${fullKey}">`;
        break;
      case 'checkbox':
        inputHtml = `<input type="checkbox" id="${inputId}" ${value ? 'checked' : ''} data-key="${fullKey}">`;
        break;
      case 'select':
        const options = def.options.map(opt => {
          const label = opt === '' ? '(System default)' : opt;
          return `<option value="${opt}" ${value === opt ? 'selected' : ''}>${label}</option>`;
        }).join('');
        inputHtml = `<select id="${inputId}" data-key="${fullKey}">${options}</select>`;
        break;
      case 'dynamic_select':
        // Dynamic select that gets options from server (e.g., model lists)
        const dynKey = def.dynamicKey;
        const dynOptions = dynamicOptions[dynKey] || [];
        const currentVal = value || '';

        // Start with current value as first option
        let dynOptionsHtml = `<option value="${escapeAttr(currentVal)}" selected>${escapeHtml(currentVal) || '(none)'}</option>`;

        // Add options from server if available
        dynOptions.forEach(opt => {
          if (opt !== currentVal) {
            dynOptionsHtml += `<option value="${escapeAttr(opt)}">${escapeHtml(opt)}</option>`;
          }
        });

        inputHtml = `<select id="${inputId}" data-key="${fullKey}" data-dynamic-key="${dynKey}">${dynOptionsHtml}</select>`;
        break;
      case 'textarea':
        inputHtml = `<textarea id="${inputId}" rows="${def.rows || 3}" placeholder="${def.placeholder || ''}" data-key="${fullKey}">${escapeHtml(value || '')}</textarea>`;
        break;
    }

    if (def.type === 'checkbox') {
      item.innerHTML = `
        ${inputHtml}
        <label for="${inputId}">${def.label} ${restartBadge}</label>
      `;
    } else {
      item.innerHTML = `
        <label for="${inputId}">${def.label} ${restartBadge}</label>
        ${inputHtml}
      `;
    }

    // Add change listener
    const input = item.querySelector('input, select, textarea');
    if (input) {
      input.addEventListener('change', () => handleSettingChange(fullKey, input));
      input.addEventListener('input', () => handleSettingChange(fullKey, input));
    }

    return item;
  }

  /**
   * Handle setting value change
   */
  function handleSettingChange(key, input) {
    changedFields.add(key);

    // Check if this field requires restart
    if (restartRequiredFields.includes(key)) {
      if (settingsElements.restartNotice) {
        settingsElements.restartNotice.classList.remove('hidden');
      }
    }
  }

  /**
   * Update secrets status indicators
   */
  function updateSecretsStatus(secrets) {
    if (!secrets) return;

    const updateStatus = (el, isSet) => {
      if (!el) return;
      el.textContent = isSet ? 'Set' : 'Not set';
      el.className = `secret-status ${isSet ? 'is-set' : 'not-set'}`;
    };

    updateStatus(settingsElements.statusOpenai, secrets.openai_api_key);
    updateStatus(settingsElements.statusClaude, secrets.claude_api_key);
    updateStatus(settingsElements.statusMqttUser, secrets.mqtt_username);
    updateStatus(settingsElements.statusMqttPass, secrets.mqtt_password);
  }

  /**
   * Collect all config values from form
   */
  function collectConfigValues() {
    const config = {};
    const inputs = settingsElements.sectionsContainer.querySelectorAll('[data-key]');

    inputs.forEach(input => {
      const key = input.dataset.key;
      const parts = key.split('.');

      let value;
      if (input.type === 'checkbox') {
        value = input.checked;
      } else if (input.type === 'number') {
        value = input.value !== '' ? parseFloat(input.value) : null;
      } else {
        value = input.value;
      }

      // Build nested structure
      let obj = config;
      for (let i = 0; i < parts.length - 1; i++) {
        if (!obj[parts[i]]) obj[parts[i]] = {};
        obj = obj[parts[i]];
      }
      obj[parts[parts.length - 1]] = value;
    });

    // AI name should always be lowercase (for wake word detection)
    if (config.general && config.general.ai_name) {
      config.general.ai_name = config.general.ai_name.toLowerCase();
    }

    return config;
  }

  /**
   * Save configuration to server
   */
  function saveConfig() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      alert('Cannot save: Not connected to server');
      return;
    }

    const config = collectConfigValues();
    console.log('Saving config:', config);

    ws.send(JSON.stringify({
      type: 'set_config',
      payload: config
    }));
  }

  /**
   * Save secrets to server
   */
  function saveSecrets() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      alert('Cannot save: Not connected to server');
      return;
    }

    const secrets = {};

    // Only send non-empty values (don't overwrite with empty)
    if (settingsElements.secretOpenai && settingsElements.secretOpenai.value) {
      secrets.openai_api_key = settingsElements.secretOpenai.value;
    }
    if (settingsElements.secretClaude && settingsElements.secretClaude.value) {
      secrets.claude_api_key = settingsElements.secretClaude.value;
    }
    if (settingsElements.secretMqttUser && settingsElements.secretMqttUser.value) {
      secrets.mqtt_username = settingsElements.secretMqttUser.value;
    }
    if (settingsElements.secretMqttPass && settingsElements.secretMqttPass.value) {
      secrets.mqtt_password = settingsElements.secretMqttPass.value;
    }

    if (Object.keys(secrets).length === 0) {
      alert('No secrets entered to save');
      return;
    }

    console.log('Saving secrets (keys only):', Object.keys(secrets));

    ws.send(JSON.stringify({
      type: 'set_secrets',
      payload: secrets
    }));

    // Clear inputs after sending
    if (settingsElements.secretOpenai) settingsElements.secretOpenai.value = '';
    if (settingsElements.secretClaude) settingsElements.secretClaude.value = '';
    if (settingsElements.secretMqttUser) settingsElements.secretMqttUser.value = '';
    if (settingsElements.secretMqttPass) settingsElements.secretMqttPass.value = '';
  }

  /**
   * Handle set_config response
   */
  function handleSetConfigResponse(payload) {
    if (payload.success) {
      console.log('Config saved successfully');

      // Check if any changed fields require restart
      const restartFields = getChangedRestartRequiredFields();
      if (restartFields.length > 0) {
        console.log('Restart required for fields:', restartFields);
        showRestartConfirmation(restartFields);
      } else {
        alert('Configuration saved successfully!');
      }

      // Clear changed fields tracking
      changedFields.clear();
    } else {
      console.error('Failed to save config:', payload.error);
      alert('Failed to save configuration: ' + (payload.error || 'Unknown error'));
    }
  }

  /**
   * Get list of changed fields that require restart
   */
  function getChangedRestartRequiredFields() {
    const restartFields = [];
    for (const field of changedFields) {
      if (restartRequiredFields.includes(field)) {
        restartFields.push(field);
      }
    }
    return restartFields;
  }

  /**
   * Show restart confirmation dialog
   */
  function showRestartConfirmation(changedRestartFields) {
    const fieldList = changedRestartFields.map(f => '  • ' + f).join('\n');
    const message = 'Configuration saved successfully!\n\n' +
      'The following changes require a restart to take effect:\n' +
      fieldList + '\n\n' +
      'Do you want to restart DAWN now?';

    if (confirm(message)) {
      requestRestart();
    }
  }

  /**
   * Request application restart
   */
  function requestRestart() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      alert('Cannot restart: Not connected to server');
      return;
    }

    console.log('Requesting application restart');
    ws.send(JSON.stringify({ type: 'restart' }));
  }

  /**
   * Handle restart response
   */
  function handleRestartResponse(payload) {
    if (payload.success) {
      console.log('Restart initiated:', payload.message);
      alert('DAWN is restarting. The page will attempt to reconnect automatically.');
    } else {
      console.error('Restart failed:', payload.error);
      alert('Failed to restart: ' + (payload.error || 'Unknown error'));
    }
  }

  /**
   * Handle system prompt response - display at top of transcript in debug mode
   */
  function handleSystemPromptResponse(payload) {
    if (!payload.success) {
      console.warn('Failed to get system prompt:', payload.error);
      return;
    }

    // Remove any existing system prompt entry
    const existing = document.getElementById('system-prompt-entry');
    if (existing) {
      existing.remove();
    }

    // Create collapsible system prompt entry
    const entry = document.createElement('div');
    entry.id = 'system-prompt-entry';
    entry.className = 'transcript-entry debug system-prompt';

    const promptLength = payload.length || payload.prompt.length;
    const tokenEstimate = Math.round(promptLength / 4); // Rough estimate

    entry.innerHTML = `
      <div class="system-prompt-header">
        <span class="system-prompt-icon">&#x2699;</span>
        <span class="system-prompt-title">System Prompt</span>
        <span class="system-prompt-stats">${promptLength.toLocaleString()} chars (~${tokenEstimate.toLocaleString()} tokens)</span>
        <span class="system-prompt-toggle">&#x25BC;</span>
      </div>
      <div class="system-prompt-content">
        <pre>${escapeHtml(payload.prompt)}</pre>
      </div>
    `;

    // Add click handler for expand/collapse
    const header = entry.querySelector('.system-prompt-header');
    header.addEventListener('click', function() {
      entry.classList.toggle('expanded');
    });

    // Insert at the top of the transcript (after placeholder if present)
    const placeholder = elements.transcript.querySelector('.transcript-placeholder');
    if (placeholder) {
      placeholder.after(entry);
    } else {
      elements.transcript.prepend(entry);
    }

    // Show only if debug mode is on
    if (!debugMode) {
      entry.style.display = 'none';
    }

    console.log(`System prompt loaded: ${promptLength} chars (~${tokenEstimate} tokens)`);
  }

  /**
   * Handle set_secrets response
   */
  function handleSetSecretsResponse(payload) {
    if (payload.success) {
      console.log('Secrets saved successfully');
      alert('Secrets saved successfully!');

      // Update status indicators
      if (payload.secrets) {
        updateSecretsStatus(payload.secrets);
      }
    } else {
      console.error('Failed to save secrets:', payload.error);
      alert('Failed to save secrets: ' + (payload.error || 'Unknown error'));
    }
  }

  // Audio device state
  let audioDevicesCache = { capture: [], playback: [] };

  /**
   * Request audio devices from server
   */
  function requestAudioDevices(backend) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      return;
    }

    console.log('Requesting audio devices for backend:', backend);
    ws.send(JSON.stringify({
      type: 'get_audio_devices',
      payload: { backend: backend }
    }));
  }

  /**
   * Handle audio devices response
   */
  function handleGetAudioDevicesResponse(payload) {
    console.log('Received audio devices:', payload);

    audioDevicesCache.capture = payload.capture_devices || [];
    audioDevicesCache.playback = payload.playback_devices || [];

    // Update capture device field
    updateAudioDeviceField('capture_device', audioDevicesCache.capture);
    updateAudioDeviceField('playback_device', audioDevicesCache.playback);
  }

  /**
   * Update an audio device field to a select with options
   */
  function updateAudioDeviceField(fieldName, devices) {
    // ID matches how createSettingField generates it (underscores preserved)
    const inputId = `setting-audio-${fieldName}`;
    const input = document.getElementById(inputId);
    console.log('updateAudioDeviceField:', fieldName, 'inputId:', inputId, 'input:', input, 'devices:', devices);
    if (!input) return;

    const currentValue = input.value || '';
    const dataKey = input.dataset.key;
    const parent = input.parentElement;

    // Replace input with select
    const select = document.createElement('select');
    select.id = inputId;
    select.dataset.key = dataKey;

    // Add default option
    const defaultOpt = document.createElement('option');
    defaultOpt.value = '';
    defaultOpt.textContent = 'Default';
    select.appendChild(defaultOpt);

    // Add device options (devices can be strings or {id, name} objects)
    devices.forEach(device => {
      const opt = document.createElement('option');
      // Handle both string devices and object devices
      const deviceId = typeof device === 'string' ? device : device.id;
      const deviceName = typeof device === 'string' ? device : (device.name || device.id);
      opt.value = deviceId;
      opt.textContent = deviceName;
      if (deviceId === currentValue) {
        opt.selected = true;
      }
      select.appendChild(opt);
    });

    // Add change listener
    select.addEventListener('change', () => handleSettingChange(dataKey, select));

    // Replace input with select
    parent.replaceChild(select, input);
  }

  /**
   * Update audio device fields based on backend selection
   */
  function updateAudioBackendState(backend) {
    // IDs match how createSettingField generates them (underscores preserved)
    const captureInput = document.getElementById('setting-audio-capture_device');
    const playbackInput = document.getElementById('setting-audio-playback_device');

    console.log('updateAudioBackendState:', backend, 'captureInput:', captureInput, 'playbackInput:', playbackInput);

    if (backend === 'auto') {
      // Grey out device fields when auto
      if (captureInput) {
        captureInput.disabled = true;
        captureInput.title = 'Device is auto-detected when backend is "auto"';
      }
      if (playbackInput) {
        playbackInput.disabled = true;
        playbackInput.title = 'Device is auto-detected when backend is "auto"';
      }
    } else {
      // Enable device fields and request available devices
      if (captureInput) {
        captureInput.disabled = false;
        captureInput.title = '';
      }
      if (playbackInput) {
        playbackInput.disabled = false;
        playbackInput.title = '';
      }

      // Request available devices for this backend
      requestAudioDevices(backend);
    }
  }

  /**
   * Toggle secret visibility
   */
  function toggleSecretVisibility(targetId) {
    const input = document.getElementById(targetId);
    if (!input) return;

    input.type = input.type === 'password' ? 'text' : 'password';
  }

  /**
   * Escape attribute value
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
   * Format a number for display, removing floating-point precision artifacts.
   * Rounds to 6 significant digits to clean up values like 0.9200000166893005 → 0.92
   */
  function formatNumber(value) {
    if (value === undefined || value === null || value === '') return '';
    const num = Number(value);
    if (isNaN(num)) return '';
    // Use toPrecision to limit significant digits, then parseFloat to remove trailing zeros
    return parseFloat(num.toPrecision(6));
  }

  /**
   * Initialize settings event listeners
   */
  function initSettingsListeners() {
    // Open button
    if (settingsElements.openBtn) {
      settingsElements.openBtn.addEventListener('click', openSettings);
    }

    // Close button
    if (settingsElements.closeBtn) {
      settingsElements.closeBtn.addEventListener('click', closeSettings);
    }

    // Overlay click to close
    if (settingsElements.overlay) {
      settingsElements.overlay.addEventListener('click', closeSettings);
    }

    // Save config button
    if (settingsElements.saveConfigBtn) {
      settingsElements.saveConfigBtn.addEventListener('click', saveConfig);
    }

    // Save secrets button
    if (settingsElements.saveSecretsBtn) {
      settingsElements.saveSecretsBtn.addEventListener('click', saveSecrets);
    }

    // Reset button
    if (settingsElements.resetBtn) {
      settingsElements.resetBtn.addEventListener('click', () => {
        if (confirm('Reset all settings to defaults? This will reload the current configuration.')) {
          requestConfig();
        }
      });
    }

    // Secret toggle buttons
    document.querySelectorAll('.secret-toggle').forEach(btn => {
      btn.addEventListener('click', () => {
        const targetId = btn.dataset.target;
        if (targetId) {
          toggleSecretVisibility(targetId);
        }
      });
    });

    // Section header toggle
    document.querySelectorAll('.section-header').forEach(header => {
      header.addEventListener('click', () => {
        header.classList.toggle('collapsed');
        const content = header.nextElementSibling;
        if (content) {
          content.classList.toggle('collapsed');
        }
      });
    });

    // Escape key to close
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape' && !settingsElements.panel.classList.contains('hidden')) {
        closeSettings();
      }
    });

    // LLM quick controls event listeners
    initLlmControls();

    // SmartThings button listeners
    if (settingsElements.stConnectBtn) {
      settingsElements.stConnectBtn.addEventListener('click', startSmartThingsOAuth);
    }
    if (settingsElements.stRefreshBtn) {
      settingsElements.stRefreshBtn.addEventListener('click', refreshSmartThingsDevices);
    }
    if (settingsElements.stDisconnectBtn) {
      settingsElements.stDisconnectBtn.addEventListener('click', disconnectSmartThings);
    }
  }

  // =============================================================================
  // LLM Runtime Controls
  // =============================================================================

  // Current LLM runtime state
  let llmRuntimeState = {
    type: 'cloud',
    provider: 'openai',
    model: '',
    openai_available: false,
    claude_available: false
  };

  /**
   * Initialize LLM quick controls
   */
  function initLlmControls() {
    const typeSelect = document.getElementById('llm-type-select');
    const providerSelect = document.getElementById('llm-provider-select');

    if (typeSelect) {
      typeSelect.addEventListener('change', () => {
        setSessionLlm({ type: typeSelect.value });
      });
    }

    if (providerSelect) {
      providerSelect.addEventListener('change', () => {
        setSessionLlm({ provider: providerSelect.value });
      });
    }
  }

  /**
   * Update LLM controls from server state
   */
  function updateLlmControls(runtime) {
    llmRuntimeState = { ...llmRuntimeState, ...runtime };

    const typeSelect = document.getElementById('llm-type-select');
    const providerSelect = document.getElementById('llm-provider-select');
    const providerGroup = document.getElementById('provider-group');
    const modelDisplay = document.getElementById('llm-model-display');

    if (typeSelect) {
      typeSelect.value = runtime.type || 'cloud';
    }

    if (providerSelect) {
      // Update available options based on API key availability
      providerSelect.innerHTML = '';

      if (runtime.openai_available) {
        const opt = document.createElement('option');
        opt.value = 'openai';
        opt.textContent = 'OpenAI';
        providerSelect.appendChild(opt);
      }

      if (runtime.claude_available) {
        const opt = document.createElement('option');
        opt.value = 'claude';
        opt.textContent = 'Claude';
        providerSelect.appendChild(opt);
      }

      // If no providers available, show disabled message
      if (!runtime.openai_available && !runtime.claude_available) {
        const opt = document.createElement('option');
        opt.value = '';
        opt.textContent = 'No API keys configured';
        opt.disabled = true;
        providerSelect.appendChild(opt);
        providerSelect.disabled = true;
      } else {
        providerSelect.disabled = false;
        // Set current value
        const currentProvider = runtime.provider?.toLowerCase() || 'openai';
        if (providerSelect.querySelector(`option[value="${currentProvider}"]`)) {
          providerSelect.value = currentProvider;
        }
      }
    }

    // Enable/disable provider selector based on LLM type
    // (Always visible, but disabled when local is selected)
    if (providerSelect) {
      if (runtime.type === 'local') {
        providerSelect.disabled = true;
        providerSelect.title = 'Provider selection not available for local LLM';
      } else if (runtime.openai_available || runtime.claude_available) {
        providerSelect.disabled = false;
        providerSelect.title = 'Switch cloud provider';
      }
    }

    // Update model display
    if (modelDisplay) {
      modelDisplay.textContent = runtime.model ? `(${runtime.model})` : '';
    }

    console.log('LLM controls updated:', runtime);
  }

  /**
   * Send per-session LLM config change to server
   */
  function setSessionLlm(changes) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      return;
    }

    console.log('Setting session LLM:', changes);
    ws.send(JSON.stringify({
      type: 'set_session_llm',
      payload: changes
    }));
  }

  /**
   * Handle per-session LLM config change response
   */
  function handleSetSessionLlmResponse(payload) {
    if (payload.success) {
      console.log('Session LLM updated:', payload);
      updateLlmControls(payload);
    } else {
      console.error('Failed to update session LLM:', payload.error);
      alert('Failed to switch LLM: ' + (payload.error || 'Unknown error'));

      // Revert controls to actual state
      if (llmRuntimeState) {
        updateLlmControls(llmRuntimeState);
      }
    }
  }

  // =============================================================================
  // SmartThings Integration
  // =============================================================================

  // SmartThings state
  let smartThingsState = {
    configured: false,
    authenticated: false,
    devices_count: 0,
    devices: [],
    auth_mode: 'none' // 'none', 'pat', 'oauth2'
  };

  /**
   * Request SmartThings status from server
   */
  function requestSmartThingsStatus() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    ws.send(JSON.stringify({ type: 'smartthings_status' }));
    console.log('Requested SmartThings status');
  }

  /**
   * Handle SmartThings status response
   */
  function handleSmartThingsStatusResponse(payload) {
    console.log('SmartThings status:', payload);
    smartThingsState.configured = payload.configured || false;
    smartThingsState.authenticated = payload.authenticated || false;
    smartThingsState.devices_count = payload.devices_count || 0;
    smartThingsState.auth_mode = payload.auth_mode || 'none';

    updateSmartThingsUI();

    // If authenticated, also get device list
    if (smartThingsState.authenticated) {
      requestSmartThingsDevices();
    }
  }

  /**
   * Helper to show/hide elements using CSS classes (CSP-compliant)
   */
  function stShow(el, displayClass) {
    if (!el) return;
    el.classList.remove('st-hidden', 'st-visible', 'st-visible-flex', 'st-visible-block');
    el.classList.add(displayClass || 'st-visible');
  }

  function stHide(el) {
    if (!el) return;
    el.classList.remove('st-visible', 'st-visible-flex', 'st-visible-block');
    el.classList.add('st-hidden');
  }

  /**
   * Update SmartThings UI based on current state
   */
  function updateSmartThingsUI() {
    const indicator = settingsElements.stStatusIndicator;
    const statusText = settingsElements.stStatusText;

    if (!indicator || !statusText) return;

    // Remove all status classes
    indicator.classList.remove('not-configured', 'configured', 'connected');

    if (!smartThingsState.configured) {
      // Not configured - show setup instructions
      indicator.classList.add('not-configured');
      statusText.textContent = 'Not Configured';

      stShow(settingsElements.stNotConfigured, 'st-visible-block');
      stHide(settingsElements.stConnectBtn);
      stHide(settingsElements.stRefreshBtn);
      stHide(settingsElements.stDisconnectBtn);
      stHide(settingsElements.stDevicesCountRow);
      stHide(settingsElements.stDevicesList);

    } else if (!smartThingsState.authenticated) {
      // Configured but not authenticated
      indicator.classList.add('configured');

      if (smartThingsState.auth_mode === 'oauth2') {
        // OAuth2 mode - show connect button
        statusText.textContent = 'Not Connected';
        stShow(settingsElements.stConnectBtn);
      } else {
        // PAT mode but not working - show error
        statusText.textContent = 'Token Invalid';
        stHide(settingsElements.stConnectBtn);
      }

      stHide(settingsElements.stNotConfigured);
      stHide(settingsElements.stRefreshBtn);
      stHide(settingsElements.stDisconnectBtn);
      stHide(settingsElements.stDevicesCountRow);
      stHide(settingsElements.stDevicesList);

    } else {
      // Authenticated - show connected state and devices
      indicator.classList.add('connected');

      if (smartThingsState.auth_mode === 'pat') {
        statusText.textContent = 'Connected (PAT)';
        // PAT is configured in secrets.toml, can't disconnect via UI
        stHide(settingsElements.stDisconnectBtn);
      } else {
        statusText.textContent = 'Connected (OAuth2)';
        stShow(settingsElements.stDisconnectBtn);
      }

      stHide(settingsElements.stNotConfigured);
      stHide(settingsElements.stConnectBtn);
      stShow(settingsElements.stRefreshBtn);
      stShow(settingsElements.stDevicesCountRow, 'st-visible-flex');
      settingsElements.stDevicesCount.textContent = smartThingsState.devices_count + ' device' + (smartThingsState.devices_count !== 1 ? 's' : '');

      if (smartThingsState.devices.length > 0) {
        stShow(settingsElements.stDevicesList, 'st-visible-block');
      }
    }
  }

  /**
   * Start SmartThings OAuth flow
   */
  function startSmartThingsOAuth() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      alert('Not connected to server');
      return;
    }

    // Get auth URL from server
    const redirectUri = window.location.origin + '/smartthings/callback';
    ws.send(JSON.stringify({
      type: 'smartthings_get_auth_url',
      payload: { redirect_uri: redirectUri }
    }));
    console.log('Requesting SmartThings auth URL');
  }

  /**
   * Handle SmartThings auth URL response
   */
  function handleSmartThingsAuthUrlResponse(payload) {
    if (payload.error) {
      alert('Failed to get auth URL: ' + payload.error);
      return;
    }

    if (payload.auth_url) {
      console.log('Opening SmartThings authorization URL');
      // Open in new window for OAuth flow
      const authWindow = window.open(payload.auth_url, 'smartthings_auth', 'width=600,height=700');

      // Listen for OAuth callback
      window.addEventListener('message', function handleOAuthCallback(event) {
        if (event.origin !== window.location.origin) return;
        if (event.data && event.data.type === 'smartthings_oauth_callback') {
          window.removeEventListener('message', handleOAuthCallback);
          if (authWindow) authWindow.close();

          if (event.data.code) {
            // Exchange code for tokens (include state for CSRF protection)
            exchangeSmartThingsCode(event.data.code, event.data.state);
          } else if (event.data.error) {
            alert('OAuth failed: ' + event.data.error);
          }
        }
      });
    }
  }

  /**
   * Exchange OAuth code for tokens
   * @param {string} code - Authorization code from OAuth callback
   * @param {string} state - State parameter for CSRF protection
   */
  function exchangeSmartThingsCode(code, state) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    const redirectUri = window.location.origin + '/smartthings/callback';
    const payload = { code: code, redirect_uri: redirectUri };
    if (state) {
      payload.state = state;
    }
    ws.send(JSON.stringify({
      type: 'smartthings_exchange_code',
      payload: payload
    }));
    console.log('Exchanging SmartThings auth code with CSRF state');
  }

  /**
   * Handle SmartThings code exchange response
   */
  function handleSmartThingsExchangeCodeResponse(payload) {
    if (payload.success) {
      console.log('SmartThings connected successfully');
      // Refresh status to show connected state
      requestSmartThingsStatus();
    } else {
      alert('Failed to connect SmartThings: ' + (payload.error || 'Unknown error'));
    }
  }

  /**
   * Request SmartThings devices list
   */
  function requestSmartThingsDevices() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    ws.send(JSON.stringify({ type: 'smartthings_list_devices' }));
    console.log('Requesting SmartThings devices');
  }

  /**
   * Refresh SmartThings devices (force refresh from API)
   */
  function refreshSmartThingsDevices() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    settingsElements.stRefreshBtn.disabled = true;
    settingsElements.stRefreshBtn.textContent = 'Refreshing...';

    ws.send(JSON.stringify({ type: 'smartthings_refresh_devices' }));
    console.log('Refreshing SmartThings devices');
  }

  /**
   * Handle SmartThings devices response
   */
  function handleSmartThingsDevicesResponse(payload) {
    // Re-enable refresh button
    if (settingsElements.stRefreshBtn) {
      settingsElements.stRefreshBtn.disabled = false;
      settingsElements.stRefreshBtn.textContent = 'Refresh Devices';
    }

    if (payload.error) {
      console.error('SmartThings devices error:', payload.error);
      return;
    }

    if (payload.devices) {
      smartThingsState.devices = payload.devices;
      smartThingsState.devices_count = payload.devices.length;
      renderSmartThingsDevices(payload.devices);
      updateSmartThingsUI();
    }
  }

  /**
   * Render SmartThings devices list
   */
  function renderSmartThingsDevices(devices) {
    const container = settingsElements.stDevicesContainer;
    if (!container) return;

    container.innerHTML = '';

    if (devices.length === 0) {
      container.innerHTML = '<div class="st-no-devices">No devices found</div>';
      return;
    }

    devices.forEach(device => {
      const deviceEl = document.createElement('div');
      deviceEl.className = 'st-device-item';

      // Get capability icons
      const caps = getCapabilityIcons(device.capabilities);

      deviceEl.innerHTML = `
        <div class="st-device-name">${escapeHtml(device.label || device.name)}</div>
        <div class="st-device-info">
          <span class="st-device-room">${escapeHtml(device.room || 'No room')}</span>
          <span class="st-device-caps">${caps}</span>
        </div>
      `;
      container.appendChild(deviceEl);
    });

    settingsElements.stDevicesList.style.display = 'block';
  }

  /**
   * Get capability icons for a device
   */
  function getCapabilityIcons(capabilities) {
    const icons = [];
    const capBits = capabilities || 0;

    if (capBits & 0x0001) icons.push('&#x1F4A1;'); // Switch (lightbulb)
    if (capBits & 0x0002) icons.push('&#x1F506;'); // Dimmer (brightness)
    if (capBits & 0x0004) icons.push('&#x1F308;'); // Color
    if (capBits & 0x0010) icons.push('&#x1F321;'); // Thermostat
    if (capBits & 0x0020) icons.push('&#x1F510;'); // Lock
    if (capBits & 0x0040) icons.push('&#x1F3C3;'); // Motion
    if (capBits & 0x0080) icons.push('&#x1F6AA;'); // Contact (door)
    if (capBits & 0x0100) icons.push('&#x1F321;'); // Temperature sensor
    if (capBits & 0x0200) icons.push('&#x1F4A7;'); // Humidity
    if (capBits & 0x2000) icons.push('&#x1FA9F;'); // Window shade

    return icons.join(' ') || '&#x2699;'; // Default: gear
  }

  /**
   * Disconnect SmartThings
   */
  function disconnectSmartThings() {
    if (!confirm('Disconnect SmartThings? This will remove your saved tokens.')) {
      return;
    }

    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    ws.send(JSON.stringify({ type: 'smartthings_disconnect' }));
    console.log('Disconnecting SmartThings');
  }

  /**
   * Handle SmartThings disconnect response
   */
  function handleSmartThingsDisconnectResponse(payload) {
    if (payload.success) {
      console.log('SmartThings disconnected');
      smartThingsState.authenticated = false;
      smartThingsState.devices = [];
      smartThingsState.devices_count = 0;
      updateSmartThingsUI();
    } else {
      alert('Failed to disconnect: ' + (payload.error || 'Unknown error'));
    }
  }

  /* =============================================================================
   * LLM Tools Configuration
   * ============================================================================= */

  let toolsConfig = [];

  /**
   * Request tools configuration from server
   */
  function requestToolsConfig() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'get_tools_config' }));
      console.log('Requesting tools config');
    }
  }

  /**
   * Handle get_tools_config response
   */
  function handleGetToolsConfigResponse(payload) {
    console.log('Tools config received:', payload);

    if (!payload.tools) {
      console.warn('No tools in response');
      return;
    }

    // Sort tools: non-armor first, then armor tools at bottom, alphabetically within each group
    toolsConfig = payload.tools.sort((a, b) => {
      const aArmor = a.armor_feature ? 1 : 0;
      const bArmor = b.armor_feature ? 1 : 0;
      if (aArmor !== bArmor) return aArmor - bArmor;
      return a.name.localeCompare(b.name);
    });
    renderToolsList();
    updateToolsTokenEstimates(payload.token_estimate);
  }

  /**
   * Handle set_tools_config response
   */
  function handleSetToolsConfigResponse(payload) {
    console.log('Set tools config response:', payload);

    const saveBtn = document.getElementById('save-tools-btn');
    if (payload.success) {
      saveBtn.textContent = 'Saved!';
      saveBtn.classList.add('saved');
      setTimeout(() => {
        saveBtn.textContent = 'Save Tool Settings';
        saveBtn.classList.remove('saved');
      }, 2000);

      // Update token estimates with new values
      if (payload.token_estimate) {
        updateToolsTokenEstimates(payload.token_estimate);
      }
    } else {
      saveBtn.textContent = 'Error!';
      setTimeout(() => {
        saveBtn.textContent = 'Save Tool Settings';
      }, 2000);
      console.error('Failed to save tools config:', payload.error);
    }
  }

  /**
   * Render the tools list with checkboxes
   */
  function renderToolsList() {
    const container = document.getElementById('tools-list');
    if (!container) return;

    if (toolsConfig.length === 0) {
      container.innerHTML = '<div class="tools-loading">No tools available</div>';
      return;
    }

    container.innerHTML = toolsConfig.map(tool => {
      const disabledClass = !tool.available ? 'disabled' : '';
      const disabledAttr = !tool.available ? 'disabled' : '';
      // Arc reactor SVG for armor features - circle with inverted triangle
      // Equilateral triangle inscribed in circle (center 12,12, radius 10)
      const armorIcon = tool.armor_feature ? `<span class="armor-icon" title="OASIS armor feature">
        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.5">
          <circle cx="12" cy="12" r="10"/>
          <polygon points="12,22 3.34,7 20.66,7" stroke-linejoin="round"/>
        </svg>
      </span>` : '';

      return `
        <div class="tool-item ${disabledClass}" data-tool="${tool.name}" title="${tool.description}">
          <div class="tool-info">
            <div class="tool-name">${tool.name}${armorIcon}</div>
          </div>
          <div class="tool-checkbox">
            <input type="checkbox" id="tool-local-${tool.name}"
                   ${tool.local ? 'checked' : ''} ${disabledAttr}
                   data-tool="${tool.name}" data-type="local">
          </div>
          <div class="tool-checkbox">
            <input type="checkbox" id="tool-remote-${tool.name}"
                   ${tool.remote ? 'checked' : ''} ${disabledAttr}
                   data-tool="${tool.name}" data-type="remote">
          </div>
        </div>
      `;
    }).join('');
  }

  /**
   * Update the token estimate display
   */
  function updateToolsTokenEstimates(estimates) {
    if (!estimates) return;

    const localEl = document.getElementById('tools-tokens-local');
    const remoteEl = document.getElementById('tools-tokens-remote');

    if (localEl) {
      localEl.textContent = `Local: ${estimates.local || 0} tokens`;
    }
    if (remoteEl) {
      remoteEl.textContent = `Remote: ${estimates.remote || 0} tokens`;
    }
  }

  /**
   * Save tools configuration
   */
  function saveToolsConfig() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      return;
    }

    const tools = [];
    const container = document.getElementById('tools-list');
    const items = container.querySelectorAll('.tool-item');

    items.forEach(item => {
      const name = item.dataset.tool;
      const localCb = item.querySelector(`input[data-type="local"]`);
      const remoteCb = item.querySelector(`input[data-type="remote"]`);

      tools.push({
        name: name,
        local: localCb ? localCb.checked : false,
        remote: remoteCb ? remoteCb.checked : false
      });
    });

    ws.send(JSON.stringify({
      type: 'set_tools_config',
      payload: { tools: tools }
    }));

    console.log('Saving tools config:', tools);
  }

  /**
   * Initialize tools section
   */
  function initToolsSection() {
    const saveBtn = document.getElementById('save-tools-btn');
    if (saveBtn) {
      saveBtn.addEventListener('click', saveToolsConfig);
    }

    // Request tools config when settings panel opens
    const settingsBtn = document.getElementById('settings-btn');
    if (settingsBtn) {
      settingsBtn.addEventListener('click', () => {
        // Small delay to let other things initialize
        setTimeout(requestToolsConfig, 100);
      });
    }

    // Initialize metrics panel
    initMetricsPanel();
  }

  // =============================================================================
  // Metrics Panel
  // =============================================================================

  let metricsInterval = null;
  let metricsVisible = false;

  function initMetricsPanel() {
    const metricsBtn = document.getElementById('metrics-btn');
    const metricsClose = document.getElementById('metrics-close');
    const metricsPanel = document.getElementById('metrics-panel');

    if (metricsBtn) {
      metricsBtn.addEventListener('click', toggleMetricsPanel);
    }
    if (metricsClose) {
      metricsClose.addEventListener('click', hideMetricsPanel);
    }
  }

  function toggleMetricsPanel() {
    const panel = document.getElementById('metrics-panel');
    const btn = document.getElementById('metrics-btn');
    if (panel.classList.contains('hidden')) {
      showMetricsPanel();
    } else {
      hideMetricsPanel();
    }
  }

  function showMetricsPanel() {
    const panel = document.getElementById('metrics-panel');
    const btn = document.getElementById('metrics-btn');
    panel.classList.remove('hidden');
    btn.classList.add('active');
    metricsVisible = true;
    requestMetrics();
    // Refresh every 2 seconds while visible
    metricsInterval = setInterval(requestMetrics, 2000);
  }

  function hideMetricsPanel() {
    const panel = document.getElementById('metrics-panel');
    const btn = document.getElementById('metrics-btn');
    panel.classList.add('hidden');
    btn.classList.remove('active');
    metricsVisible = false;
    if (metricsInterval) {
      clearInterval(metricsInterval);
      metricsInterval = null;
    }
  }

  function requestMetrics() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'get_metrics' }));
    }
  }

  function handleMetricsResponse(payload) {
    if (!metricsVisible) return;

    // Session stats
    const uptime = payload.session?.uptime_seconds || 0;
    const hours = Math.floor(uptime / 3600);
    const mins = Math.floor((uptime % 3600) / 60);
    const uptimeStr = hours > 0 ? `${hours}h ${mins}m` : `${mins}m`;
    setText('m-uptime', uptimeStr);

    const queries = payload.session?.queries_total || 0;
    const cloud = payload.session?.queries_cloud || 0;
    const local = payload.session?.queries_local || 0;
    setText('m-queries', `${queries} (${cloud}c/${local}l)`);
    setText('m-errors', payload.session?.errors || 0);
    setText('m-bargeins', payload.session?.bargeins || 0);

    // Tokens
    const tc = payload.tokens || {};
    setText('m-tokens-cloud', `${formatNum(tc.cloud_input)}/${formatNum(tc.cloud_output)}`);
    setText('m-tokens-local', `${formatNum(tc.local_input)}/${formatNum(tc.local_output)}`);
    setText('m-tokens-cached', formatNum(tc.cached));

    // Last pipeline
    const last = payload.last || {};
    setText('m-last-asr', formatMs(last.asr_ms));
    setText('m-last-llm', formatMs(last.llm_total_ms));
    setText('m-last-tts', formatMs(last.tts_ms));

    // Averages
    const avg = payload.averages || {};
    setText('m-avg-asr', formatMs(avg.asr_ms));
    setText('m-avg-llm', formatMs(avg.llm_total_ms));
    setText('m-avg-tts', formatMs(avg.tts_ms));

    // System
    const vad = payload.state?.vad_probability || 0;
    setText('m-vad', `${(vad * 100).toFixed(0)}%`);

    const aec = payload.aec || {};
    let aecStr = aec.enabled ? (aec.calibrated ? `${aec.delay_ms}ms` : 'uncal') : 'off';
    setText('m-aec', aecStr);
  }

  function setText(id, value) {
    const el = document.getElementById(id);
    if (el) el.textContent = value;
  }

  function formatMs(ms) {
    if (ms === undefined || ms === null || ms === 0) return '--';
    if (ms < 1000) return `${Math.round(ms)}ms`;
    return `${(ms / 1000).toFixed(1)}s`;
  }

  function formatNum(n) {
    if (n === undefined || n === null) return '--';
    if (n >= 1000000) return `${(n / 1000000).toFixed(1)}M`;
    if (n >= 1000) return `${(n / 1000).toFixed(1)}K`;
    return String(n);
  }

  // Start when DOM is ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

  // Expose visualization toggle for testing/debugging
  // Usage: DAWN.toggleVisualization() or DAWN.setVisualization('bars'|'waveform')
  window.DAWN = window.DAWN || {};
  window.DAWN.toggleVisualization = toggleVisualizationMode;
  window.DAWN.setVisualization = function(mode) {
    if (mode === 'bars' || mode === 'waveform') {
      visualizationMode = mode;
      if (mode === 'bars') {
        showBarMode();
      } else {
        showWaveformMode();
      }
      console.log('Visualization mode set to:', mode);
    }
  };
  window.DAWN.getVisualizationMode = function() { return visualizationMode; };

  // FFT debug toggle
  window.DAWN.toggleFFTDebug = function() {
    fftDebugState.enabled = !fftDebugState.enabled;
    fftDebugState.peakMax = 0;  // Reset peak on toggle
    const el = document.getElementById('fft-debug');
    if (el) el.style.display = fftDebugState.enabled ? 'block' : 'none';
    console.log('FFT debug:', fftDebugState.enabled ? 'ON' : 'OFF');
    return fftDebugState.enabled;
  };

})();
