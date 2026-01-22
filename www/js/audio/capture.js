/**
 * DAWN Audio Capture Module
 * Microphone input, PCM conversion, and transmission
 *
 * Uses AudioWorklet when available (modern browsers) with
 * ScriptProcessorNode fallback for older browsers.
 *
 * Usage:
 *   DawnAudioCapture.init()           // Check audio support
 *   DawnAudioCapture.start()          // Start recording
 *   DawnAudioCapture.stop()           // Stop recording
 *   DawnAudioCapture.setCallbacks({ onMicButton, onError, getOpusEncoder })
 */
(function (global) {
   'use strict';

   // Audio capture state
   let audioContext = null;
   let mediaStream = null;
   let audioProcessor = null; // ScriptProcessorNode or AudioWorkletNode
   let mediaStreamSource = null;
   let audioChunkMs = 200; // Updated from server config
   let useWorklet = false; // Whether AudioWorklet is being used
   let workletLoaded = false; // Whether worklet module has been loaded into context

   // Callbacks (set by dawn.js)
   let callbacks = {
      onMicButton: null, // (recording: boolean) => void
      onError: null, // (message: string) => void
      getOpusEncoder: null, // () => { ready: boolean, worker: Worker } | null
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
               autoGainControl: true,
            },
         });

         // Reuse existing AudioContext or create new one
         const AudioContextClass = window.AudioContext || window.webkitAudioContext;
         if (!audioContext || audioContext.state === 'closed') {
            audioContext = new AudioContextClass({ sampleRate: DawnConfig.AUDIO_SAMPLE_RATE });
            workletLoaded = false; // New context needs worklet loaded
            console.log(
               'AudioContext created: requested',
               DawnConfig.AUDIO_SAMPLE_RATE,
               'Hz, actual',
               audioContext.sampleRate,
               'Hz'
            );
         } else if (audioContext.state === 'suspended') {
            await audioContext.resume();
            console.log('AudioContext resumed');
         }

         // Create source from microphone stream
         mediaStreamSource = audioContext.createMediaStreamSource(mediaStream);

         // Try to use AudioWorklet, fall back to ScriptProcessorNode
         const workletAvailable = await trySetupWorklet();
         if (!workletAvailable) {
            setupScriptProcessor();
         }

         DawnState.setIsRecording(true);
         updateMicButton(true);
      } catch (e) {
         console.error('Failed to start recording:', e);
         const msg =
            e.name === 'NotAllowedError'
               ? 'Microphone access denied. Please allow microphone access.'
               : 'Failed to access microphone: ' + e.message;
         if (callbacks.onError) {
            callbacks.onError(msg);
         }
      }
   }

   /**
    * Try to set up AudioWorklet for recording
    * @returns {Promise<boolean>} True if worklet was set up successfully
    */
   async function trySetupWorklet() {
      // Check if AudioWorklet is supported
      if (typeof AudioWorklet === 'undefined' || !audioContext.audioWorklet) {
         return false;
      }

      try {
         // Ensure context is running before loading worklet (MDN recommendation)
         await audioContext.resume();

         // Load worklet module only once per context (calling again throws error)
         // Note: Worklet path is tightly coupled to server's static file serving
         if (!workletLoaded) {
            await audioContext.audioWorklet.addModule('/js/audio/capture-worklet.js');
            workletLoaded = true;
         }

         // Create AudioWorkletNode
         audioProcessor = new AudioWorkletNode(audioContext, 'pcm-capture-processor');

         // Handle messages from worklet
         audioProcessor.port.onmessage = function (event) {
            const msg = event.data;
            if (msg.type === 'audio' && DawnState.getIsRecording()) {
               // msg.data is already Int16Array
               sendAudioChunk(msg.data.buffer);
            }
         };

         // Calculate target samples for configured chunk duration
         const targetSamples = Math.floor((audioContext.sampleRate * audioChunkMs) / 1000);

         // Configure worklet
         audioProcessor.port.postMessage({
            type: 'config',
            sampleRate: audioContext.sampleRate,
            targetSamples: targetSamples,
         });

         // Tell worklet to start recording
         audioProcessor.port.postMessage({ type: 'start' });

         // Connect: source -> worklet
         mediaStreamSource.connect(audioProcessor);

         useWorklet = true;
         console.log(
            'Recording started (AudioWorklet):',
            targetSamples,
            'samples per chunk (~' + audioChunkMs + 'ms)'
         );
         return true;
      } catch (e) {
         console.warn('AudioWorklet setup failed, falling back to ScriptProcessorNode:', e);
         return false;
      }
   }

   /**
    * Set up ScriptProcessorNode for recording (fallback)
    */
   function setupScriptProcessor() {
      // Calculate buffer size (must be power of 2)
      const desiredSamples = Math.floor((DawnConfig.AUDIO_SAMPLE_RATE * audioChunkMs) / 1000);
      const bufferSize = Math.pow(2, Math.ceil(Math.log2(desiredSamples)));

      // Create ScriptProcessorNode
      audioProcessor = audioContext.createScriptProcessor(bufferSize, 1, 1);

      audioProcessor.onaudioprocess = function (e) {
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
            pcmData[i] = s < 0 ? s * 0x8000 : s * 0x7fff;
         }

         // Send audio chunk to server
         sendAudioChunk(pcmData.buffer);
      };

      // Connect nodes: source -> processor -> destination
      mediaStreamSource.connect(audioProcessor);
      audioProcessor.connect(audioContext.destination);

      useWorklet = false;
      console.log(
         'Recording started (ScriptProcessorNode fallback):',
         bufferSize,
         'samples per chunk (~' +
            ((bufferSize / DawnConfig.AUDIO_SAMPLE_RATE) * 1000).toFixed(0) +
            'ms)'
      );
   }

   /**
    * Stop recording and send end marker
    */
   function stop() {
      if (!DawnState.getIsRecording()) {
         return;
      }

      DawnState.setIsRecording(false);

      // Tell worklet to stop (will flush remaining buffer)
      if (useWorklet && audioProcessor && audioProcessor.port) {
         audioProcessor.port.postMessage({ type: 'stop' });
      }

      // Stop audio processing
      if (audioProcessor) {
         audioProcessor.disconnect();
         audioProcessor = null;
      }

      // Disconnect source
      if (mediaStreamSource) {
         mediaStreamSource.disconnect();
         mediaStreamSource = null;
      }

      // Suspend audio context (don't close - reuse for next recording session)
      // This avoids the overhead of creating a new context each time
      if (audioContext && audioContext.state === 'running') {
         audioContext.suspend();
      }

      // Stop microphone stream
      if (mediaStream) {
         mediaStream.getTracks().forEach((track) => track.stop());
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
      setCallbacks: setCallbacks,
   };
})(window);
