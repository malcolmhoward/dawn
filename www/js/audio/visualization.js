/**
 * DAWN Audio Visualization Module
 * FFT visualization, waveform/bar rendering, hesitation ring
 *
 * Usage:
 *   DawnVisualization.init()                    // Initialize rings and bars
 *   DawnVisualization.startFFT()                // Start FFT animation
 *   DawnVisualization.stopFFT()                 // Stop FFT animation
 *   DawnVisualization.onTokenEvent()            // Track token timing
 *   DawnVisualization.toggleMode()              // Toggle waveform/bars
 *   DawnVisualization.drawDefault()             // Reset to idle state
 *   DawnVisualization.startHesitation()         // Start hesitation animation
 *   DawnVisualization.stopHesitation()          // Stop hesitation animation
 *   DawnVisualization.renderThroughputRing(fillLevel)  // Update throughput ring
 */
(function (global) {
   'use strict';

   // =============================================================================
   // Visualization Constants
   // =============================================================================

   // Multi-ring configuration (viewBox is 240x240, center at 120,120)
   const WAVEFORM_CENTER = 120;

   // Inner ring (FFT audio)
   const WAVEFORM_BASE_RADIUS = 60;
   const WAVEFORM_SPIKE_HEIGHT = 15;
   const WAVEFORM_POINTS = 64;

   // Bar visualization configuration (radial EQ style)
   const BAR_COUNT = 128;
   const BAR_INNER_RADIUS = 58;
   const BAR_MAX_OUTER_RADIUS = 83;
   const BAR_GAP_DEGREES = 0.5;
   const BAR_COLOR_LOW = { r: 34, g: 211, b: 238 }; // #22d3ee (cyan)
   const BAR_COLOR_HIGH = { r: 245, g: 158, b: 11 }; // #f59e0b (amber)

   // Middle ring (throughput)
   const THROUGHPUT_RADIUS = 87;
   const THROUGHPUT_SEGMENTS = 64;

   // Outer ring (hesitation)
   const HESITATION_RADIUS = 107;
   const HESITATION_SEGMENTS = 64;

   // Trail configuration
   const TRAIL_LENGTH = 5;
   const TRAIL_SAMPLE_RATE = 10;

   // =============================================================================
   // Visualization State
   // =============================================================================

   let fftAnimationId = null;
   let waveformHistory = [];
   let frameCount = 0;

   let visualizationMode = 'bars';
   let barElements = null;
   let barTrailElements = [];
   let barDataHistory = [];
   let cachedBarGroup = null; // Cache bar group reference (M4)

   let fftDebugState = {
      enabled: false,
      peakMax: 0,
   };

   // Segment cache to avoid DOM thrashing
   const segmentCache = new WeakMap();

   // Load-based ring transition state
   let loadTransitionState = {
      highLoadStartMs: 0,
      sustainedThreshold: 0.4,
      sustainedDelay: 500,
      isMiddleStrained: false,
   };

   // =============================================================================
   // FFT Data Processing
   // =============================================================================

   /**
    * Process FFT data for waveform visualization
    */
   function processFFTData(fftData) {
      const halfPoints = WAVEFORM_POINTS / 2;
      const processed = new Float32Array(WAVEFORM_POINTS);

      if (!fftData || fftData.length === 0) {
         return processed;
      }

      const usableBins = Math.floor(fftData.length * 0.4);
      const startBin = 1;

      const halfValues = new Float32Array(halfPoints);
      let maxVal = 0;

      for (let i = 0; i < halfPoints; i++) {
         const t = i / halfPoints;
         const logT = Math.pow(t, 0.6);
         const binIndex = startBin + Math.floor(logT * (usableBins - startBin));

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

      const threshold = 3;
      if (maxVal > threshold) {
         for (let i = 0; i < halfPoints; i++) {
            halfValues[i] = Math.max(0, halfValues[i] - threshold) / (maxVal - threshold);
            halfValues[i] = Math.pow(halfValues[i], 0.7);
         }
      }

      for (let i = 0; i < halfPoints; i++) {
         processed[i] = halfValues[i];
         processed[WAVEFORM_POINTS - 1 - i] = halfValues[i];
      }

      return processed;
   }

   /**
    * Process FFT data for bar visualization
    */
   function processFFTDataForBars(fftData) {
      const processed = new Float32Array(BAR_COUNT);

      if (!fftData || fftData.length === 0) {
         return processed;
      }

      const usableBins = Math.floor(fftData.length * 0.4);
      const startBin = 1;
      let maxVal = 0;

      for (let i = 0; i < BAR_COUNT; i++) {
         const t = i / BAR_COUNT;
         const logT = Math.pow(t, 0.6);
         const binIndex = startBin + Math.floor(logT * (usableBins - startBin));

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

      const threshold = 3;
      if (maxVal > threshold) {
         for (let i = 0; i < BAR_COUNT; i++) {
            processed[i] = Math.max(0, processed[i] - threshold) / (maxVal - threshold);
            processed[i] = Math.pow(processed[i], 0.7);
         }
      }

      return processed;
   }

   // =============================================================================
   // Waveform Rendering
   // =============================================================================

   /**
    * Generate SVG path for circular waveform
    */
   function generateWaveformPath(processedData, scale = 1.0) {
      const points = [];
      const numPoints = WAVEFORM_POINTS;

      for (let i = 0; i < numPoints; i++) {
         const angle = (i / numPoints) * Math.PI * 2 - Math.PI / 2;
         const value = processedData && processedData.length > i ? processedData[i] : 0;
         const spikeHeight = value * WAVEFORM_SPIKE_HEIGHT * scale;
         const radius = (WAVEFORM_BASE_RADIUS + spikeHeight) * scale;
         const x = WAVEFORM_CENTER + Math.cos(angle) * radius;
         const y = WAVEFORM_CENTER + Math.sin(angle) * radius;
         points.push({ x, y });
      }

      if (points.length === 0) return '';

      let path = `M ${points[0].x.toFixed(1)} ${points[0].y.toFixed(1)}`;

      for (let i = 1; i < points.length; i++) {
         const curr = points[i];
         const prev = points[i - 1];
         const cpX = (prev.x + curr.x) / 2;
         const cpY = (prev.y + curr.y) / 2;
         path += ` Q ${prev.x.toFixed(1)} ${prev.y.toFixed(1)} ${cpX.toFixed(1)} ${cpY.toFixed(1)}`;
      }

      const last = points[points.length - 1];
      const first = points[0];
      const cpX = (last.x + first.x) / 2;
      const cpY = (last.y + first.y) / 2;
      path += ` Q ${last.x.toFixed(1)} ${last.y.toFixed(1)} ${cpX.toFixed(1)} ${cpY.toFixed(1)}`;
      path += ' Z';

      return path;
   }

   // =============================================================================
   // Bar Rendering
   // =============================================================================

   /**
    * Interpolate bar color based on intensity
    */
   function interpolateBarColor(intensity) {
      const t = Math.max(0, Math.min(1, intensity));

      if (DawnTheme.current() === 'terminal') {
         const alpha = 0.4 + t * 0.6;
         return `rgba(127, 255, 127, ${alpha.toFixed(2)})`;
      }

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

      const anglePerBar = 360 / BAR_COUNT;
      const circumference = 2 * Math.PI * BAR_MAX_OUTER_RADIUS;
      const barWidth = circumference / BAR_COUNT - 1;
      line.setAttribute('stroke-width', Math.max(1, barWidth * 0.85).toFixed(1));

      const angleDeg = index * anglePerBar - 90;
      const angleRad = (angleDeg * Math.PI) / 180;

      const x1 = WAVEFORM_CENTER + Math.cos(angleRad) * BAR_INNER_RADIUS;
      const y1 = WAVEFORM_CENTER + Math.sin(angleRad) * BAR_INNER_RADIUS;

      line.setAttribute('x1', x1.toFixed(1));
      line.setAttribute('y1', y1.toFixed(1));
      line.setAttribute('x2', x1.toFixed(1));
      line.setAttribute('y2', y1.toFixed(1));
      line.setAttribute('stroke', `rgb(${BAR_COLOR_LOW.r},${BAR_COLOR_LOW.g},${BAR_COLOR_LOW.b})`);

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
    * Initialize bar elements
    */
   function initializeBarElements() {
      const container = DawnElements.ringFft;
      if (!container) return;

      if (DawnElements.waveformPath) {
         DawnElements.waveformPath.style.display = 'none';
      }
      DawnElements.waveformTrails.forEach((trail) => {
         if (trail) trail.style.display = 'none';
      });

      // Use cached reference or find/create bar group (M4)
      if (!cachedBarGroup) {
         cachedBarGroup = container.querySelector('.bar-group');
         if (!cachedBarGroup) {
            cachedBarGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
            cachedBarGroup.classList.add('bar-group');
            container.appendChild(cachedBarGroup);
         }
      }
      cachedBarGroup.innerHTML = '';

      barTrailElements = [];
      for (let t = TRAIL_LENGTH - 1; t >= 0; t--) {
         const trailBars = [];
         for (let i = 0; i < BAR_COUNT; i++) {
            trailBars.push(createBarElement(cachedBarGroup, i, true, t));
         }
         barTrailElements.unshift(trailBars);
      }

      barElements = [];
      for (let i = 0; i < BAR_COUNT; i++) {
         barElements.push(createBarElement(cachedBarGroup, i, false));
      }

      barDataHistory = [];
      console.log(`Initialized ${BAR_COUNT} bar elements with ${TRAIL_LENGTH} trail layers`);
   }

   /**
    * Render bars with current data
    */
   function renderBars(processedData, scale = 1.0) {
      if (!barElements || barElements.length === 0) return;

      const anglePerBar = 360 / BAR_COUNT;

      for (let i = 0; i < BAR_COUNT; i++) {
         const value = processedData[i] || 0;
         const angleDeg = i * anglePerBar - 90;
         const angleRad = (angleDeg * Math.PI) / 180;

         const barLength =
            BAR_INNER_RADIUS + value * (BAR_MAX_OUTER_RADIUS - BAR_INNER_RADIUS) * scale;

         const x2 = WAVEFORM_CENTER + Math.cos(angleRad) * barLength;
         const y2 = WAVEFORM_CENTER + Math.sin(angleRad) * barLength;

         barElements[i].setAttribute('x2', x2.toFixed(1));
         barElements[i].setAttribute('y2', y2.toFixed(1));
         barElements[i].setAttribute('stroke', interpolateBarColor(value));
      }
   }

   /**
    * Render bar trails
    */
   function renderBarTrails() {
      for (let t = 0; t < barTrailElements.length && t < barDataHistory.length; t++) {
         const trailData = barDataHistory[t];
         const trailBars = barTrailElements[t];
         const anglePerBar = 360 / BAR_COUNT;
         const baseOpacity = 0.5 - t * 0.08;

         for (let i = 0; i < BAR_COUNT; i++) {
            const value = trailData[i] || 0;
            const angleDeg = i * anglePerBar - 90;
            const angleRad = (angleDeg * Math.PI) / 180;

            const barLength = BAR_INNER_RADIUS + value * (BAR_MAX_OUTER_RADIUS - BAR_INNER_RADIUS);
            const x2 = WAVEFORM_CENTER + Math.cos(angleRad) * barLength;
            const y2 = WAVEFORM_CENTER + Math.sin(angleRad) * barLength;

            trailBars[i].setAttribute('x2', x2.toFixed(1));
            trailBars[i].setAttribute('y2', y2.toFixed(1));
            trailBars[i].setAttribute('stroke', interpolateBarColor(value * 0.7));
            trailBars[i].style.opacity = (baseOpacity * (1 - value * 0.3)).toFixed(2);
         }
      }
   }

   // =============================================================================
   // Ring Rendering
   // =============================================================================

   /**
    * Render a segmented arc
    */
   function renderSegmentedArc(container, radius, segments, fillLevel = 0, jitter = null) {
      if (!container) return;

      const gapAngle = 0.02;
      const segmentAngle = (Math.PI * 2) / segments - gapAngle;
      const activeSegments = Math.floor(segments * fillLevel);

      let paths = segmentCache.get(container);
      if (!paths || paths.length !== segments) {
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

      for (let i = 0; i < segments; i++) {
         const startAngle = (i / segments) * Math.PI * 2 - Math.PI / 2;
         const endAngle = startAngle + segmentAngle;
         const r = jitter && jitter[i] ? radius + jitter[i] : radius;

         const x1 = WAVEFORM_CENTER + Math.cos(startAngle) * r;
         const y1 = WAVEFORM_CENTER + Math.sin(startAngle) * r;
         const x2 = WAVEFORM_CENTER + Math.cos(endAngle) * r;
         const y2 = WAVEFORM_CENTER + Math.sin(endAngle) * r;

         paths[i].setAttribute(
            'd',
            `M ${x1.toFixed(1)} ${y1.toFixed(1)} A ${r} ${r} 0 0 1 ${x2.toFixed(1)} ${y2.toFixed(1)}`
         );
         paths[i].style.opacity = i < activeSegments ? '0.8' : '0.22';
      }
   }

   // =============================================================================
   // Hesitation Ring (Token Timing Variance)
   // =============================================================================

   /**
    * Calculate running stddev using Welford's algorithm
    */
   function getRunningStddev() {
      const n = DawnState.hesitationState.dtWindow.length;
      if (n === 0) return 0;
      const mean = DawnState.hesitationState.runningSum / n;
      const variance = DawnState.hesitationState.runningSumSq / n - mean * mean;
      return Math.sqrt(Math.max(0, variance));
   }

   /**
    * Smooth noise for hesitation jitter
    */
   function smoothNoise(segmentId, time) {
      const phase1 = segmentId * 0.37;
      const phase2 = segmentId * 0.73;
      const phase3 = segmentId * 1.17;

      const n1 = Math.sin(time * 2.0 + phase1);
      const n2 = Math.sin(time * 3.7 + phase2) * 0.5;
      const n3 = Math.sin(time * 5.3 + phase3) * 0.25;

      return (n1 + n2 + n3) / 1.75;
   }

   /**
    * Handle token event for hesitation tracking
    */
   function onTokenEvent() {
      const nowMs = performance.now();

      if (DawnState.hesitationState.tPrevMs > 0) {
         const dtMs = nowMs - DawnState.hesitationState.tPrevMs;

         if (DawnState.hesitationState.dtWindow.length >= DawnState.hesitationState.windowSize) {
            const oldValue = DawnState.hesitationState.dtWindow.shift();
            DawnState.hesitationState.runningSum -= oldValue;
            DawnState.hesitationState.runningSumSq -= oldValue * oldValue;
         }

         DawnState.hesitationState.dtWindow.push(dtMs);
         DawnState.hesitationState.runningSum += dtMs;
         DawnState.hesitationState.runningSumSq += dtMs * dtMs;

         const std = getRunningStddev();
         const loadRaw = (std - 3) / (20 - 3);
         const load = Math.max(0, Math.min(1, loadRaw));

         DawnState.hesitationState.loadSmooth +=
            0.2 * (load - DawnState.hesitationState.loadSmooth);
      }

      DawnState.hesitationState.tPrevMs = nowMs;
      DawnState.hesitationState.lastTokenMs = nowMs;
   }

   /**
    * Update hesitation ring with jitter
    */
   function updateHesitationRing(updateTelemetryFn) {
      const nowMs = performance.now();
      const timeSec = nowMs / 1000;

      if (nowMs - DawnState.hesitationState.lastTokenMs > 300) {
         DawnState.hesitationState.loadSmooth *= 0.95;
         if (DawnState.hesitationState.loadSmooth < 0.01) {
            DawnState.hesitationState.loadSmooth = 0;
         }
      }

      const jitter = new Array(HESITATION_SEGMENTS);
      const maxJitter = 3;

      for (let i = 0; i < HESITATION_SEGMENTS; i++) {
         const noise = smoothNoise(i, timeSec);
         jitter[i] = noise * maxJitter * DawnState.hesitationState.loadSmooth;
      }

      renderSegmentedArc(
         DawnElements.ringHesitation,
         HESITATION_RADIUS,
         HESITATION_SEGMENTS,
         1.0,
         jitter
      );
      updateMiddleRingStrain(nowMs);

      if (updateTelemetryFn) {
         updateTelemetryFn();
      }
   }

   /**
    * Update middle ring strain state
    */
   function updateMiddleRingStrain(nowMs) {
      const load = DawnState.hesitationState.loadSmooth;
      const threshold = loadTransitionState.sustainedThreshold;
      const delay = loadTransitionState.sustainedDelay;

      if (load > threshold) {
         if (loadTransitionState.highLoadStartMs === 0) {
            loadTransitionState.highLoadStartMs = nowMs;
         }

         if (nowMs - loadTransitionState.highLoadStartMs > delay) {
            if (!loadTransitionState.isMiddleStrained) {
               DawnElements.ringThroughput.classList.add('strained');
               loadTransitionState.isMiddleStrained = true;
            }
         }
      } else {
         loadTransitionState.highLoadStartMs = 0;
         if (loadTransitionState.isMiddleStrained) {
            DawnElements.ringThroughput.classList.remove('strained');
            loadTransitionState.isMiddleStrained = false;
         }
      }
   }

   // =============================================================================
   // Default State Rendering
   // =============================================================================

   /**
    * Draw default waveform (idle state)
    */
   function drawDefaultWaveform() {
      if (visualizationMode === 'bars') {
         drawDefaultBars();
      } else {
         const path = generateWaveformPath(null, 1.0);
         if (DawnElements.waveformPath) {
            DawnElements.waveformPath.setAttribute('d', path);
         }
         for (const trail of DawnElements.waveformTrails) {
            if (trail) {
               trail.setAttribute('d', path);
            }
         }
      }
      waveformHistory = [];
      barDataHistory = [];
      frameCount = 0;
   }

   /**
    * Draw default bars (idle state)
    */
   function drawDefaultBars() {
      if (!barElements || barElements.length === 0) return;

      const anglePerBar = 360 / BAR_COUNT;
      const minBarLength = BAR_INNER_RADIUS + 2;

      for (let i = 0; i < BAR_COUNT; i++) {
         const angleDeg = i * anglePerBar - 90;
         const angleRad = (angleDeg * Math.PI) / 180;

         const x2 = WAVEFORM_CENTER + Math.cos(angleRad) * minBarLength;
         const y2 = WAVEFORM_CENTER + Math.sin(angleRad) * minBarLength;

         barElements[i].setAttribute('x2', x2.toFixed(1));
         barElements[i].setAttribute('y2', y2.toFixed(1));
         barElements[i].setAttribute('stroke', interpolateBarColor(0));
      }

      for (let t = 0; t < barTrailElements.length; t++) {
         for (let i = 0; i < BAR_COUNT; i++) {
            const angleDeg = i * anglePerBar - 90;
            const angleRad = (angleDeg * Math.PI) / 180;
            const x2 = WAVEFORM_CENTER + Math.cos(angleRad) * minBarLength;
            const y2 = WAVEFORM_CENTER + Math.sin(angleRad) * minBarLength;
            barTrailElements[t][i].setAttribute('x2', x2.toFixed(1));
            barTrailElements[t][i].setAttribute('y2', y2.toFixed(1));
         }
      }
   }

   // =============================================================================
   // Mode Switching
   // =============================================================================

   function showWaveformMode() {
      if (DawnElements.waveformPath) {
         DawnElements.waveformPath.style.display = '';
      }
      DawnElements.waveformTrails.forEach((trail) => {
         if (trail) trail.style.display = '';
      });

      // Use cached reference (M4)
      if (cachedBarGroup) {
         cachedBarGroup.style.display = 'none';
      }
   }

   function showBarMode() {
      if (DawnElements.waveformPath) {
         DawnElements.waveformPath.style.display = 'none';
      }
      DawnElements.waveformTrails.forEach((trail) => {
         if (trail) trail.style.display = 'none';
      });

      // Use cached reference (M4)
      if (cachedBarGroup) {
         cachedBarGroup.style.display = '';
      }

      if (!barElements || barElements.length === 0) {
         initializeBarElements();
      }
   }

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
   // Animation Control
   // =============================================================================

   /**
    * Start FFT visualization
    */
   function startFFTVisualization() {
      const analyser = DawnAudioPlayback.getAnalyser();
      const fftData = DawnAudioPlayback.getFFTData();
      if (!DawnElements.ringContainer || !analyser || !fftData) {
         console.warn('FFT visualization: missing requirements');
         return;
      }

      console.log('Starting FFT visualization for DAWN speech');
      DawnElements.ringContainer.classList.add('fft-active');

      function animate() {
         const analyser = DawnAudioPlayback.getAnalyser();
         const fftData = DawnAudioPlayback.getFFTData();
         if (!analyser || !fftData || !DawnAudioPlayback.isPlaying()) {
            return;
         }

         analyser.getByteFrequencyData(fftData);

         let sum = 0;
         let maxRaw = 0;
         for (let i = 0; i < fftData.length; i++) {
            sum += fftData[i];
            if (fftData[i] > maxRaw) maxRaw = fftData[i];
         }
         const average = sum / fftData.length;
         const normalizedLevel = average / 255;

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

         if (visualizationMode === 'bars') {
            const barData = processFFTDataForBars(fftData);
            renderBars(barData, 1.0);

            frameCount++;
            if (frameCount >= TRAIL_SAMPLE_RATE) {
               frameCount = 0;
               barDataHistory.unshift(Array.from(barData));
               if (barDataHistory.length > TRAIL_LENGTH) {
                  barDataHistory.pop();
               }
            }

            renderBarTrails();
         } else {
            const processedData = processFFTData(fftData);

            if (DawnElements.waveformPath) {
               const path = generateWaveformPath(processedData, 1.0);
               DawnElements.waveformPath.setAttribute('d', path);

               frameCount++;
               if (frameCount >= TRAIL_SAMPLE_RATE) {
                  frameCount = 0;
                  waveformHistory.unshift(path);
                  if (waveformHistory.length > TRAIL_LENGTH) {
                     waveformHistory.pop();
                  }
               }

               for (let i = 0; i < DawnElements.waveformTrails.length; i++) {
                  if (DawnElements.waveformTrails[i] && waveformHistory[i]) {
                     DawnElements.waveformTrails[i].setAttribute('d', waveformHistory[i]);
                  }
               }
            }
         }

         const coreScale = 1.0 + normalizedLevel * 0.25;
         const coreOpacity = 0.6 + normalizedLevel * 0.4;
         if (DawnElements.ringInner) {
            DawnElements.ringInner.style.transform = `scale(${coreScale.toFixed(3)})`;
            DawnElements.ringInner.style.opacity = coreOpacity.toFixed(2);
         }

         // Skip animation when page is hidden to save CPU (M1)
         if (!document.hidden) {
            fftAnimationId = requestAnimationFrame(animate);
         }
      }

      fftAnimationId = requestAnimationFrame(animate);
   }

   /**
    * Stop FFT visualization
    */
   function stopFFTVisualization() {
      if (fftAnimationId) {
         cancelAnimationFrame(fftAnimationId);
         fftAnimationId = null;
      }

      drawDefaultWaveform();

      if (DawnElements.ringInner) {
         DawnElements.ringInner.style.transform = '';
         DawnElements.ringInner.style.opacity = '';
      }
      if (DawnElements.ringContainer) {
         DawnElements.ringContainer.classList.remove('fft-active');
      }
      console.log('Stopped FFT visualization');
   }

   /**
    * Start hesitation animation
    */
   function startHesitationAnimation(updateTelemetryFn) {
      if (DawnState.hesitationState.animationId) return;

      function animate() {
         updateHesitationRing(updateTelemetryFn);
         // Skip animation when page is hidden to save CPU (M1)
         if (!document.hidden) {
            DawnState.hesitationState.animationId = requestAnimationFrame(animate);
         }
      }

      DawnState.hesitationState.animationId = requestAnimationFrame(animate);
   }

   /**
    * Stop hesitation animation
    */
   function stopHesitationAnimation() {
      if (DawnState.hesitationState.animationId) {
         cancelAnimationFrame(DawnState.hesitationState.animationId);
         DawnState.hesitationState.animationId = null;
      }

      DawnState.hesitationState.dtWindow = [];
      DawnState.hesitationState.tPrevMs = 0;
      DawnState.hesitationState.loadSmooth = 0;
      DawnState.hesitationState.runningSum = 0;
      DawnState.hesitationState.runningSumSq = 0;

      renderSegmentedArc(DawnElements.ringHesitation, HESITATION_RADIUS, HESITATION_SEGMENTS, 0);
   }

   // =============================================================================
   // Initialization
   // =============================================================================

   /**
    * Initialize rings and visualization
    */
   function init() {
      renderSegmentedArc(DawnElements.ringThroughput, THROUGHPUT_RADIUS, THROUGHPUT_SEGMENTS, 0);
      renderSegmentedArc(DawnElements.ringHesitation, HESITATION_RADIUS, HESITATION_SEGMENTS, 0);

      if (visualizationMode === 'bars') {
         initializeBarElements();
      }
   }

   /**
    * Render throughput ring
    */
   function renderThroughputRing(fillLevel) {
      renderSegmentedArc(
         DawnElements.ringThroughput,
         THROUGHPUT_RADIUS,
         THROUGHPUT_SEGMENTS,
         fillLevel
      );
   }

   /**
    * Toggle FFT debug display
    */
   function toggleFFTDebug() {
      fftDebugState.enabled = !fftDebugState.enabled;
      const debugEl = document.getElementById('fft-debug');
      if (debugEl) {
         debugEl.style.display = fftDebugState.enabled ? 'block' : 'none';
      }
      if (!fftDebugState.enabled) {
         fftDebugState.peakMax = 0;
      }
      return fftDebugState.enabled;
   }

   /**
    * Get current visualization mode
    */
   function getMode() {
      return visualizationMode;
   }

   // Resume animations when page becomes visible again (M1)
   document.addEventListener('visibilitychange', function () {
      if (!document.hidden) {
         // Restart FFT if it was running
         if (DawnAudioPlayback.getAnalyser() && !fftAnimationId) {
            fftAnimationId = requestAnimationFrame(function animate() {
               // Re-trigger the visualization loop
               global.DawnVisualization.startFFT();
            });
         }
         // Restart hesitation if it was running
         if (DawnState.hesitationState.startTime && !DawnState.hesitationState.animationId) {
            startHesitationAnimation(DawnMetrics.updatePanel);
         }
      }
   });

   // Expose globally
   global.DawnVisualization = {
      init: init,
      startFFT: startFFTVisualization,
      stopFFT: stopFFTVisualization,
      drawDefault: drawDefaultWaveform,
      toggleMode: toggleVisualizationMode,
      getMode: getMode,
      onTokenEvent: onTokenEvent,
      startHesitation: startHesitationAnimation,
      stopHesitation: stopHesitationAnimation,
      renderThroughputRing: renderThroughputRing,
      toggleFFTDebug: toggleFFTDebug,
   };
})(window);
