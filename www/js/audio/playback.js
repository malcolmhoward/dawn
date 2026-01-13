/**
 * DAWN Audio Playback Module
 * TTS audio playback with FFT analyser for visualization
 *
 * Usage:
 *   DawnAudioPlayback.queueAudio(pcmData)     // Queue PCM data for playback
 *   DawnAudioPlayback.play()                  // Start playback of queued audio
 *   DawnAudioPlayback.isPlaying()             // Check if currently playing
 *   DawnAudioPlayback.getAnalyser()           // Get analyser node for FFT
 *   DawnAudioPlayback.getFFTData()            // Get FFT data array
 *   DawnAudioPlayback.setCallbacks({ onPlaybackStart, onPlaybackEnd })
 */
(function (global) {
   'use strict';

   // Audio playback state
   let playbackContext = null;
   let playbackAnalyser = null;
   let fftDataArray = null;
   let audioChunks = [];
   let audioPlaybackQueue = [];
   let isPlayingAudio = false;

   // Callbacks
   let callbacks = {
      onPlaybackStart: null, // () => void - called when playback starts
      onPlaybackEnd: null, // () => void - called when all audio finished
   };

   /**
    * Queue raw PCM data for playback
    * @param {Uint8Array} pcmData - Raw PCM audio bytes
    */
   function queueAudio(pcmData) {
      audioChunks.push(pcmData);
   }

   /**
    * Play accumulated PCM audio chunks via Web Audio API
    * Format: 16-bit signed integer PCM, 16kHz, mono
    * If audio is currently playing, queues the new audio for later
    */
   async function play() {
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
      audioChunks = []; // Clear for next audio stream

      if (isPlayingAudio) {
         // Queue this audio to play after current finishes
         // Bound queue to prevent memory exhaustion on rapid TTS
         const MAX_QUEUE_SIZE = 10;
         if (audioPlaybackQueue.length >= MAX_QUEUE_SIZE) {
            console.warn('Audio playback queue full, dropping oldest segment');
            audioPlaybackQueue.shift();
         }
         console.log(
            'Audio playing, queuing',
            alignedLength,
            'bytes for later (queue size:',
            audioPlaybackQueue.length + 1,
            ')'
         );
         audioPlaybackQueue.push(combinedBuffer);
         return;
      }

      // Play immediately
      await playAudioBuffer(combinedBuffer);
   }

   /**
    * Play a raw PCM buffer
    * @param {Uint8Array} buffer - Raw PCM audio bytes
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
            int16Data[i] = dataView.getInt16(i * 2, true); // true = little-endian
         }

         console.log(
            'Playing',
            numSamples,
            'samples (',
            (numSamples / DawnConfig.AUDIO_SAMPLE_RATE).toFixed(2),
            'seconds)'
         );

         // Create playback AudioContext if needed
         const AudioContext = window.AudioContext || window.webkitAudioContext;
         if (!playbackContext || playbackContext.state === 'closed') {
            playbackContext = new AudioContext({ sampleRate: DawnConfig.AUDIO_SAMPLE_RATE });

            // Create analyser for FFT visualization (gives DAWN "life" when speaking)
            playbackAnalyser = playbackContext.createAnalyser();
            playbackAnalyser.fftSize = 256; // Frequency bins for detail
            playbackAnalyser.smoothingTimeConstant = 0.4; // Lower = more responsive, higher = smoother
            playbackAnalyser.minDecibels = -55; // Match TTS quiet parts (~-50dB)
            playbackAnalyser.maxDecibels = -10; // Match TTS peaks (~-10dB)
            fftDataArray = new Uint8Array(playbackAnalyser.frequencyBinCount);
         }

         // Resume context if suspended (browser autoplay policy)
         if (playbackContext.state === 'suspended') {
            console.log('Resuming suspended audio context...');
            await playbackContext.resume();
         }

         // Create AudioBuffer
         const audioBuffer = playbackContext.createBuffer(
            1,
            numSamples,
            DawnConfig.AUDIO_SAMPLE_RATE
         );
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

         // Notify playback start
         if (callbacks.onPlaybackStart) {
            callbacks.onPlaybackStart();
         }

         source.onended = function () {
            console.log('Audio chunk finished');

            // Check if there's queued audio to play next
            if (audioPlaybackQueue.length > 0) {
               const nextBuffer = audioPlaybackQueue.shift();
               console.log(
                  'Playing next queued audio (remaining in queue:',
                  audioPlaybackQueue.length,
                  ')'
               );
               playAudioBuffer(nextBuffer);
            } else {
               // No more audio - mark playback complete
               console.log('All audio playback finished');
               isPlayingAudio = false;
               if (callbacks.onPlaybackEnd) {
                  callbacks.onPlaybackEnd();
               }
            }
         };

         console.log('Starting audio playback...');
         source.start(0);
      } catch (e) {
         console.error('Failed to play audio:', e);
         isPlayingAudio = false;
         if (callbacks.onPlaybackEnd) {
            callbacks.onPlaybackEnd();
         }
         // Try to play next in queue even on error
         if (audioPlaybackQueue.length > 0) {
            const nextBuffer = audioPlaybackQueue.shift();
            playAudioBuffer(nextBuffer);
         }
      }
   }

   /**
    * Check if audio is currently playing
    * @returns {boolean}
    */
   function isPlaying() {
      return isPlayingAudio;
   }

   /**
    * Get the analyser node for FFT visualization
    * @returns {AnalyserNode|null}
    */
   function getAnalyser() {
      return playbackAnalyser;
   }

   /**
    * Get the FFT data array
    * @returns {Uint8Array|null}
    */
   function getFFTData() {
      return fftDataArray;
   }

   /**
    * Set callbacks
    * @param {Object} cbs - Callback functions
    */
   function setCallbacks(cbs) {
      if (cbs.onPlaybackStart) callbacks.onPlaybackStart = cbs.onPlaybackStart;
      if (cbs.onPlaybackEnd) callbacks.onPlaybackEnd = cbs.onPlaybackEnd;
   }

   // Expose globally
   global.DawnAudioPlayback = {
      queueAudio: queueAudio,
      play: play,
      isPlaying: isPlaying,
      getAnalyser: getAnalyser,
      getFFTData: getFFTData,
      setCallbacks: setCallbacks,
   };
})(window);
