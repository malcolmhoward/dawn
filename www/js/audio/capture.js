/**
 * DAWN Audio Capture Module
 * Microphone input, PCM conversion, and transmission
 *
 * Usage:
 *   DawnAudioCapture.init()           // Check audio support
 *   DawnAudioCapture.start()          // Start recording
 *   DawnAudioCapture.stop()           // Stop recording
 *   DawnAudioCapture.setCallbacks({ onMicButton, onError, getOpusEncoder })
 */
(function(global) {
  'use strict';

  // Audio capture state
  let audioContext = null;
  let mediaStream = null;
  let audioProcessor = null;
  let audioChunkMs = 200;  // Updated from server config

  // Callbacks (set by dawn.js)
  let callbacks = {
    onMicButton: null,      // (recording: boolean) => void
    onError: null,          // (message: string) => void
    getOpusEncoder: null    // () => { ready: boolean, worker: Worker } | null
  };

  /**
   * Initialize audio and check for microphone support
   * @returns {Promise<{supported: boolean, reason: string|null}>}
   */
  async function init() {
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

      DawnState.setAudioSupported(true);
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
  async function start() {
    if (DawnState.getIsRecording()) {
      console.warn('Already recording');
      return;
    }

    if (!DawnState.getAudioSupported()) {
      console.error('Audio not supported');
      return;
    }

    try {
      // Request microphone access
      mediaStream = await navigator.mediaDevices.getUserMedia({
        audio: {
          sampleRate: DawnConfig.AUDIO_SAMPLE_RATE,
          channelCount: DawnConfig.AUDIO_CHANNELS,
          echoCancellation: true,
          noiseSuppression: true,
          autoGainControl: true
        }
      });

      // Create audio context at target sample rate
      const AudioContext = window.AudioContext || window.webkitAudioContext;
      audioContext = new AudioContext({ sampleRate: DawnConfig.AUDIO_SAMPLE_RATE });
      console.log('AudioContext created: requested', DawnConfig.AUDIO_SAMPLE_RATE, 'Hz, actual', audioContext.sampleRate, 'Hz');

      // Create source from microphone stream
      const source = audioContext.createMediaStreamSource(mediaStream);

      // Create ScriptProcessorNode for audio processing
      // Note: ScriptProcessorNode is deprecated but AudioWorklet requires HTTPS
      const desiredSamples = Math.floor(DawnConfig.AUDIO_SAMPLE_RATE * audioChunkMs / 1000);
      const bufferSize = Math.pow(2, Math.ceil(Math.log2(desiredSamples)));
      console.log('Audio buffer size:', bufferSize, 'samples (', bufferSize / DawnConfig.AUDIO_SAMPLE_RATE * 1000, 'ms)');
      audioProcessor = audioContext.createScriptProcessor(bufferSize, 1, 1);

      audioProcessor.onaudioprocess = function(e) {
        // Clear output buffer to prevent feedback/garbage
        const outputData = e.outputBuffer.getChannelData(0);
        outputData.fill(0);

        if (!DawnState.getIsRecording()) return;

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

      DawnState.setIsRecording(true);
      updateMicButton(true);
      console.log('Recording started');

    } catch (e) {
      console.error('Failed to start recording:', e);
      const msg = e.name === 'NotAllowedError'
        ? 'Microphone access denied. Please allow microphone access.'
        : 'Failed to access microphone: ' + e.message;
      if (callbacks.onError) {
        callbacks.onError(msg);
      }
    }
  }

  /**
   * Stop recording and send end marker
   */
  function stop() {
    if (!DawnState.getIsRecording()) {
      return;
    }

    DawnState.setIsRecording(false);

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
    if (!DawnWS.isConnected()) {
      return;
    }

    // Check if Opus encoding is available
    const opus = callbacks.getOpusEncoder ? callbacks.getOpusEncoder() : null;
    if (opus && opus.ready && opus.worker && DawnWS.getCapabilitiesSynced()) {
      // Encode via Opus worker - it will call sendOpusData when ready
      const pcmData = new Int16Array(pcmBuffer);
      opus.worker.postMessage({ type: 'encode', data: pcmData }, [pcmData.buffer]);
    } else {
      // Send raw PCM: [type byte][PCM data]
      const payload = new Uint8Array(1 + pcmBuffer.byteLength);
      payload[0] = DawnConfig.WS_BIN_AUDIO_IN;
      payload.set(new Uint8Array(pcmBuffer), 1);
      DawnWS.sendBinary(payload.buffer);
    }
  }

  /**
   * Send audio end marker to server
   */
  function sendAudioEnd() {
    if (!DawnWS.isConnected()) {
      return;
    }

    const payload = new Uint8Array([DawnConfig.WS_BIN_AUDIO_IN_END]);
    DawnWS.sendBinary(payload.buffer);
    console.log('Sent audio end marker');
  }

  /**
   * Update mic button visual state
   */
  function updateMicButton(recording) {
    if (callbacks.onMicButton) {
      callbacks.onMicButton(recording);
    } else if (DawnElements.micBtn) {
      DawnElements.micBtn.classList.toggle('recording', recording);
      DawnElements.micBtn.textContent = recording ? 'Stop' : 'Mic';
      DawnElements.micBtn.title = recording ? 'Stop recording' : 'Start recording';
    }
  }

  /**
   * Set audio chunk interval (called when config received from server)
   */
  function setAudioChunkMs(ms) {
    audioChunkMs = ms;
  }

  /**
   * Set callbacks
   */
  function setCallbacks(cbs) {
    if (cbs.onMicButton) callbacks.onMicButton = cbs.onMicButton;
    if (cbs.onError) callbacks.onError = cbs.onError;
    if (cbs.getOpusEncoder) callbacks.getOpusEncoder = cbs.getOpusEncoder;
  }

  // Expose globally
  global.DawnAudioCapture = {
    init: init,
    start: start,
    stop: stop,
    setAudioChunkMs: setAudioChunkMs,
    setCallbacks: setCallbacks
  };

})(window);
