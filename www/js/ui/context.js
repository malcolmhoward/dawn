/**
 * DAWN Context Gauge Module
 * Rainbow arc gauge showing LLM context/token usage
 *
 * Usage:
 *   DawnContextGauge.init()                    // Initialize gauge segments
 *   DawnContextGauge.render(usage)             // Render gauge at usage % (0-100)
 *   DawnContextGauge.updateDisplay(payload)    // Update from server context message
 */
(function(global) {
  'use strict';

  // =============================================================================
  // Constants
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

  // Callbacks
  let callbacks = {
    onUpdate: null  // (usage) => void - called when context updates
  };

  // =============================================================================
  // Color Interpolation
  // =============================================================================

  /**
   * Interpolate gauge color based on position (0-1)
   * @param {number} t - Position along gauge (0 = start, 1 = end)
   * @returns {string} RGB color string
   */
  function interpolateColor(t) {
    const r = Math.round(GAUGE_COLOR_LOW.r + (GAUGE_COLOR_HIGH.r - GAUGE_COLOR_LOW.r) * t);
    const g = Math.round(GAUGE_COLOR_LOW.g + (GAUGE_COLOR_HIGH.g - GAUGE_COLOR_LOW.g) * t);
    const b = Math.round(GAUGE_COLOR_LOW.b + (GAUGE_COLOR_HIGH.b - GAUGE_COLOR_LOW.b) * t);
    return `rgb(${r},${g},${b})`;
  }

  // =============================================================================
  // Gauge Initialization
  // =============================================================================

  /**
   * Initialize context gauge segments - rainbow arc, thin to thick
   */
  function init() {
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
      const color = interpolateColor(t);
      path.setAttribute('fill', color);
      path.style.opacity = '0.2';  // Dimmed when inactive

      container.appendChild(path);
    }
  }

  // =============================================================================
  // Gauge Rendering
  // =============================================================================

  /**
   * Update context gauge arc with current usage
   * @param {number} usage - Usage percentage (0-100)
   */
  function render(usage) {
    const container = document.getElementById('context-gauge-segments');
    if (!container) return;

    const segments = container.querySelectorAll('.gauge-segment');
    const activeCount = Math.ceil((usage / 100) * GAUGE_SEGMENTS);

    segments.forEach((seg, i) => {
      const t = parseFloat(seg.dataset.position) || (i / (GAUGE_SEGMENTS - 1));
      const color = interpolateColor(t);

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
  }

  /**
   * Update context/token usage display from server payload
   * @param {Object} payload - Context info {current, max, usage, threshold}
   * @param {Function} updateTelemetryFn - Optional function to update telemetry panel
   */
  function updateDisplay(payload, updateTelemetryFn) {
    const contextDisplay = document.getElementById('context-display');
    const contextText = document.getElementById('context-text');
    const contextPercent = document.getElementById('context-gauge-percent');

    if (!contextDisplay) {
      console.warn('Context display elements not found');
      return;
    }

    const { current, max, usage } = payload;

    // Update the gauge arc
    render(usage);

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

    // Update metricsState
    if (typeof DawnState !== 'undefined' && DawnState.metricsState) {
      DawnState.metricsState.context_percent = usage;
    }

    // Call telemetry update if provided
    if (updateTelemetryFn) {
      updateTelemetryFn();
    }

    // Sync mini status bar context (when visualizer collapsed)
    if (typeof DawnElements !== 'undefined') {
      if (DawnElements.miniContextValue) {
        DawnElements.miniContextValue.textContent = Math.round(usage) + '%';
      }
      if (DawnElements.miniContext) {
        DawnElements.miniContext.classList.remove('warning', 'danger');
        if (usage > 90) {
          DawnElements.miniContext.classList.add('danger');
        } else if (usage > 70) {
          DawnElements.miniContext.classList.add('warning');
        }
      }
    }

    // Debug logging
    if (typeof DawnState !== 'undefined' && DawnState.getDebugMode && DawnState.getDebugMode()) {
      console.log(`Context update: ${current}/${max} tokens (${usage.toFixed(1)}%)`);
    }

    // Notify callback
    if (callbacks.onUpdate) {
      callbacks.onUpdate(usage);
    }
  }

  /**
   * Set callbacks
   * @param {Object} cbs - Callback functions
   */
  function setCallbacks(cbs) {
    if (cbs.onUpdate) callbacks.onUpdate = cbs.onUpdate;
  }

  // =============================================================================
  // Export Module
  // =============================================================================

  global.DawnContextGauge = {
    init: init,
    render: render,
    updateDisplay: updateDisplay,
    setCallbacks: setCallbacks
  };

})(window);
