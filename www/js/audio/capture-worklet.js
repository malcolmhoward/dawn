/**
 * DAWN Audio Capture Worklet Processor
 * Runs on dedicated audio thread for low-latency PCM capture
 */

class PcmCaptureProcessor extends AudioWorkletProcessor {
   constructor() {
      super();
      this.sampleRate = 48000;
      this.targetSamples = 9600;
      this.buffer = new Float32Array(this.targetSamples);
      this.bufferIndex = 0;
      this.isRecording = false;

      this.port.onmessage = (event) => {
         const msg = event.data;
         if (msg.type === 'config') {
            if (msg.sampleRate) this.sampleRate = msg.sampleRate;
            if (msg.targetSamples) {
               this.targetSamples = msg.targetSamples;
               this.buffer = new Float32Array(this.targetSamples);
               this.bufferIndex = 0;
            }
         } else if (msg.type === 'start') {
            this.isRecording = true;
            this.bufferIndex = 0;
         } else if (msg.type === 'stop') {
            this.isRecording = false;
            if (this.bufferIndex > 0) this.flushBuffer();
         }
      };
   }

   flushBuffer() {
      if (this.bufferIndex === 0) return;
      const pcmData = new Int16Array(this.bufferIndex);
      for (let i = 0; i < this.bufferIndex; i++) {
         const s = Math.max(-1, Math.min(1, this.buffer[i]));
         pcmData[i] = s < 0 ? s * 0x8000 : s * 0x7fff;
      }
      this.port.postMessage({ type: 'audio', data: pcmData }, [pcmData.buffer]);
      this.bufferIndex = 0;
   }

   process(inputs) {
      const input = inputs[0];
      if (!input || !input[0] || !this.isRecording) return true;

      const channelData = input[0];
      for (let i = 0; i < channelData.length; i++) {
         // Check bounds BEFORE writing to prevent buffer overflow
         if (this.bufferIndex >= this.targetSamples) this.flushBuffer();
         this.buffer[this.bufferIndex++] = channelData[i];
      }
      return true;
   }
}

registerProcessor('pcm-capture-processor', PcmCaptureProcessor);
