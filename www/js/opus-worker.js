/**
 * DAWN Opus Audio Worker
 *
 * Web Worker for encoding and decoding Opus audio using WebCodecs API.
 * Operates at 48kHz (Opus native rate). Server handles resampling to 16kHz for ASR.
 * Falls back to raw PCM passthrough if WebCodecs is not available.
 *
 * Message Types:
 * - init: Initialize encoder/decoder
 * - encode: Encode PCM samples to Opus frames
 * - decode: Decode Opus frames to PCM samples
 * - destroy: Clean up resources
 */

'use strict';

// Configuration - 48kHz is Opus native rate, server resamples for ASR
const SAMPLE_RATE = 48000;
const CHANNELS = 1;
const BITRATE = 24000;
const FRAME_DURATION_US = 20000; // 20ms in microseconds

let encoder = null;
let decoder = null;
let webCodecsSupported = false;
let pendingEncodedChunks = [];
let pendingDecodedSamples = [];

/**
 * Check if WebCodecs AudioEncoder/AudioDecoder are available
 */
function checkWebCodecsSupport() {
   return (
      typeof AudioEncoder !== 'undefined' &&
      typeof AudioDecoder !== 'undefined' &&
      typeof EncodedAudioChunk !== 'undefined'
   );
}

/**
 * Reset decoder state after error
 */
async function resetDecoder() {
   if (decoder) {
      try {
         decoder.reset();
         decoder.configure({
            codec: 'opus',
            sampleRate: SAMPLE_RATE,
            numberOfChannels: CHANNELS,
         });
      } catch (e) {
         console.error('Opus worker: Failed to reset decoder:', e);
      }
   }
}

/**
 * Initialize encoder and decoder using WebCodecs API
 */
async function init() {
   webCodecsSupported = checkWebCodecsSupport();

   if (!webCodecsSupported) {
      console.warn('Opus worker: WebCodecs not supported, falling back to PCM passthrough');
      postMessage({ type: 'init_done', webcodecs: false });
      return;
   }

   try {
      // Check if Opus is supported at 48kHz
      const decoderSupport = await AudioDecoder.isConfigSupported({
         codec: 'opus',
         sampleRate: SAMPLE_RATE,
         numberOfChannels: CHANNELS,
      });

      if (!decoderSupport.supported) {
         console.warn('Opus worker: Opus decoder not supported at', SAMPLE_RATE, 'Hz');
         webCodecsSupported = false;
         postMessage({ type: 'init_done', webcodecs: false });
         return;
      }

      // Create decoder
      decoder = new AudioDecoder({
         output: handleDecodedAudio,
         error: async (e) => {
            console.error('Opus decoder error:', e);
            postMessage({ type: 'error', error: 'Decoder error: ' + e.message });
            // Reset decoder state on error to recover
            await resetDecoder();
         },
      });

      decoder.configure({
         codec: 'opus',
         sampleRate: SAMPLE_RATE,
         numberOfChannels: CHANNELS,
      });

      // Check encoder support
      const encoderSupport = await AudioEncoder.isConfigSupported({
         codec: 'opus',
         sampleRate: SAMPLE_RATE,
         numberOfChannels: CHANNELS,
         bitrate: BITRATE,
      });

      if (!encoderSupport.supported) {
         console.warn('Opus worker: Opus encoder not supported, decode-only mode');
      } else {
         // Create encoder
         encoder = new AudioEncoder({
            output: handleEncodedChunk,
            error: (e) => {
               console.error('Opus encoder error:', e);
               postMessage({ type: 'error', error: 'Encoder error: ' + e.message });
            },
         });

         encoder.configure({
            codec: 'opus',
            sampleRate: SAMPLE_RATE,
            numberOfChannels: CHANNELS,
            bitrate: BITRATE,
            opus: {
               application: 'voip',
               signal: 'voice',
               frameDuration: FRAME_DURATION_US,
               complexity: 5,
               usedtx: true,
            },
         });
      }

      console.log('Opus worker: WebCodecs initialized at', SAMPLE_RATE, 'Hz');
      postMessage({ type: 'init_done', webcodecs: true, sampleRate: SAMPLE_RATE });
   } catch (e) {
      console.error('Opus worker: WebCodecs init failed:', e);
      webCodecsSupported = false;
      postMessage({ type: 'init_done', webcodecs: false, error: e.message });
   }
}

/**
 * Handle decoded audio from WebCodecs AudioDecoder
 * Output is at 48kHz - no resampling needed
 */
function handleDecodedAudio(audioData) {
   try {
      const numFrames = audioData.numberOfFrames;
      const samples = new Float32Array(numFrames);

      // Copy samples from AudioData
      audioData.copyTo(samples, { planeIndex: 0 });

      // Convert Float32 to Int16
      const int16Samples = new Int16Array(numFrames);
      for (let i = 0; i < numFrames; i++) {
         const s = Math.max(-1, Math.min(1, samples[i]));
         int16Samples[i] = s < 0 ? s * 32768 : s * 32767;
      }

      pendingDecodedSamples.push(int16Samples);
      audioData.close();
   } catch (e) {
      console.error('Error handling decoded audio:', e);
   }
}

/**
 * Handle encoded chunk from WebCodecs AudioEncoder
 */
function handleEncodedChunk(chunk, metadata) {
   try {
      const data = new Uint8Array(chunk.byteLength);
      chunk.copyTo(data);
      pendingEncodedChunks.push(data);
   } catch (e) {
      console.error('Error handling encoded chunk:', e);
   }
}

/**
 * Encode PCM samples to Opus
 * Input: Int16Array of PCM samples at 48kHz
 * Output: Uint8Array with length-prefixed Opus frames
 */
async function encode(pcmData) {
   if (!encoder) {
      postMessage({ type: 'encoded', data: new Uint8Array(0), raw: true });
      return;
   }

   try {
      pendingEncodedChunks = [];

      // Convert Int16 to Float32 for WebCodecs
      const float32Data = new Float32Array(pcmData.length);
      for (let i = 0; i < pcmData.length; i++) {
         float32Data[i] = pcmData[i] / 32768.0;
      }

      // Create AudioData at 48kHz
      const audioData = new AudioData({
         format: 'f32',
         sampleRate: SAMPLE_RATE,
         numberOfFrames: pcmData.length,
         numberOfChannels: CHANNELS,
         timestamp: 0,
         data: float32Data,
      });

      encoder.encode(audioData);
      audioData.close();

      await encoder.flush();

      if (pendingEncodedChunks.length === 0) {
         postMessage({ type: 'encoded', data: new Uint8Array(0) });
         return;
      }

      // Build length-prefixed output
      let totalLen = 0;
      for (const pkt of pendingEncodedChunks) {
         totalLen += 2 + pkt.length;
      }

      const output = new Uint8Array(totalLen);
      let offset = 0;

      for (const pkt of pendingEncodedChunks) {
         output[offset++] = pkt.length & 0xff;
         output[offset++] = (pkt.length >> 8) & 0xff;
         output.set(pkt, offset);
         offset += pkt.length;
      }

      pendingEncodedChunks = [];
      postMessage({ type: 'encoded', data: output }, [output.buffer]);
   } catch (e) {
      console.error('Encode failed:', e);
      postMessage({ type: 'error', error: 'Encode failed: ' + e.message });
   }
}

/**
 * Decode Opus frames to PCM samples
 * Input: Uint8Array with length-prefixed Opus frames
 * Output: Int16Array of PCM samples at 48kHz
 */
async function decode(opusData) {
   if (!decoder) {
      postMessage({ type: 'error', error: 'Decoder not initialized' });
      return;
   }

   try {
      pendingDecodedSamples = [];

      let offset = 0;
      let timestamp = 0;
      const frameDuration = 20000; // 20ms in microseconds

      // Parse length-prefixed frames
      while (offset + 2 <= opusData.length) {
         const frameLen = opusData[offset] | (opusData[offset + 1] << 8);
         offset += 2;

         if (frameLen === 0 || frameLen > 1500 || offset + frameLen > opusData.length) {
            console.warn('Opus worker: invalid frame length', frameLen);
            break;
         }

         const frame = opusData.subarray(offset, offset + frameLen);
         offset += frameLen;

         const chunk = new EncodedAudioChunk({
            type: 'key',
            timestamp: timestamp,
            data: frame,
         });

         decoder.decode(chunk);
         timestamp += frameDuration;
      }

      await decoder.flush();

      if (pendingDecodedSamples.length === 0) {
         postMessage({ type: 'decoded', data: new Int16Array(0) });
         return;
      }

      // Combine all samples
      let totalSamples = 0;
      for (const chunk of pendingDecodedSamples) {
         totalSamples += chunk.length;
      }

      const output = new Int16Array(totalSamples);
      let writeOffset = 0;
      for (const chunk of pendingDecodedSamples) {
         output.set(chunk, writeOffset);
         writeOffset += chunk.length;
      }

      pendingDecodedSamples = [];
      postMessage({ type: 'decoded', data: output }, [output.buffer]);
   } catch (e) {
      console.error('Decode failed:', e);
      postMessage({ type: 'error', error: 'Decode failed: ' + e.message });
      // Reset decoder on error
      await resetDecoder();
   }
}

/**
 * Clean up resources
 */
function destroy() {
   if (encoder) {
      encoder.close();
      encoder = null;
   }
   if (decoder) {
      decoder.close();
      decoder = null;
   }
   postMessage({ type: 'destroyed' });
}

/**
 * Handle messages from main thread
 */
onmessage = async function (e) {
   const msg = e.data;

   switch (msg.type) {
      case 'init':
         await init();
         break;

      case 'encode':
         await encode(msg.data);
         break;

      case 'decode':
         await decode(msg.data);
         break;

      case 'destroy':
         destroy();
         break;

      default:
         console.warn('Opus worker: unknown message type', msg.type);
   }
};

// Signal that worker script is loaded
postMessage({ type: 'ready' });
