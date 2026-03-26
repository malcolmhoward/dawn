# Interactive widget guidelines

Use HTML type for widgets with user controls. JavaScript executes after
the content is fully streamed/rendered.

## Controls
- Sliders: `<input type="range">` for continuous values
- Toggles: checkbox styled as switch for on/off states
- Buttons: for discrete actions or stepping through states
- Dropdowns: `<select>` for choosing between modes

## State Management
Use JavaScript variables. No localStorage or sessionStorage (not
available in sandboxed iframe).

```javascript
let state = { temperature: 50, heating: true };
function update() { /* re-render based on state */ }
```

## Inline SVG in HTML
For interactive diagrams, embed SVG directly in the HTML:

```html
<div>
  <svg width="100%" viewBox="0 0 680 400">
    <!-- diagram content, same rules as static SVG -->
  </svg>
  <div style="display:flex; gap:12px; margin-top:8px">
    <label>Temperature
      <input type="range" min="0" max="100" value="50"
             oninput="setTemp(this.value)">
    </label>
  </div>
</div>
```

## Animation Rules
- CSS `@keyframes` only. Animate `transform` and `opacity` only.
- Keep loops under 2 seconds.
- Always wrap in `@media (prefers-reduced-motion: no-preference)`.
- Animations show behavior (flow, rotation), not decoration.

## sendPrompt() for Drill-Down
Use clickable elements to trigger follow-up conversations:
```html
<button onclick="sendPrompt('Explain the cooling system in detail')">
  Learn more
</button>
```

## Stepper Pattern (for cycles/processes)
When the subject has stages that loop, build a stepper instead of
a ring diagram:

```html
<div id="stepper">
  <div id="stage-content"><!-- filled by JS --></div>
  <div style="display:flex; gap:8px; margin-top:12px">
    <button onclick="prev()">Back</button>
    <span id="dots"></span>
    <button onclick="next()">Next</button>
  </div>
</div>
```

Next on the last stage wraps to the first — that IS the loop.

## External Libraries
Load from DAWN's local server only (no CDN — offline-first):
- Chart.js: `<script src="/js/vendor/chart.umd.js"></script>`
- D3 and Three.js: not bundled yet — use SVG-only approaches instead
