/**
 * DAWN Visual Renderer Module
 * Detects <dawn-visual> tags in transcript messages and renders them
 * as sandboxed iframes with theme CSS injection and height auto-sizing.
 *
 * Usage:
 *   DawnVisualRender.processElement(textEl)  // Scan for <dawn-visual> tags
 */
(function (global) {
   'use strict';

   /* Track known visual iframe contentWindows for postMessage source validation */
   var knownVisualWindows = new WeakSet();

   /* Regex to match <dawn-visual title="..." type="svg|html">...</dawn-visual> */
   var VISUAL_TAG_RE =
      /<dawn-visual\s+title="([^"]*)"\s+type="(svg|html)">([\s\S]*?)<\/dawn-visual>/g;

   /**
    * Build theme CSS that reads actual computed variables from the parent document.
    * This is injected into every iframe so LLM-generated code can use CSS variables.
    */
   function buildThemeCSS() {
      var style = getComputedStyle(document.documentElement);
      var get = function (name) {
         return style.getPropertyValue(name).trim();
      };

      return (
         ':root {\n' +
         '  --color-bg-primary: ' +
         get('--bg-primary') +
         ';\n' +
         '  --color-bg-secondary: ' +
         get('--bg-secondary') +
         ';\n' +
         '  --color-bg-tertiary: ' +
         get('--bg-tertiary') +
         ';\n' +
         '  --color-text-primary: ' +
         get('--text-primary') +
         ';\n' +
         '  --color-text-secondary: ' +
         get('--text-secondary') +
         ';\n' +
         '  --color-text-tertiary: ' +
         get('--text-tertiary') +
         ';\n' +
         '  --color-border: ' +
         get('--border-color') +
         ';\n' +
         '  --color-accent: ' +
         get('--accent') +
         ';\n' +
         '  --font-sans: Inter, system-ui, sans-serif;\n' +
         '  --font-mono: "JetBrains Mono", monospace;\n' +
         '}\n' +
         'body {\n' +
         '  font-family: var(--font-sans);\n' +
         '  color: var(--color-text-primary);\n' +
         '  background: transparent;\n' +
         '  margin: 0; padding: 0;\n' +
         '}\n'
      );
   }

   /**
    * Build the pre-built CSS classes referenced in _core.md guidelines.
    * These map short class names to styled SVG/HTML elements.
    */
   function buildVisualClasses() {
      return (
         /* Text classes */
         '.t { font-family: var(--font-sans); font-size: 14px; fill: var(--color-text-primary); }\n' +
         '.ts { font-family: var(--font-sans); font-size: 12px; fill: var(--color-text-secondary); }\n' +
         '.th { font-family: var(--font-sans); font-size: 14px; font-weight: 500; fill: var(--color-text-primary); }\n' +
         /* Box class */
         '.box { fill: var(--color-bg-secondary); stroke: var(--color-border); stroke-width: 0.5; }\n' +
         /* Arrow class */
         '.arr { stroke: var(--color-text-secondary); stroke-width: 1.5; fill: none; }\n' +
         /* Clickable node */
         '.node { cursor: pointer; }\n' +
         '.node:hover { opacity: 0.85; }\n' +
         /* Color ramp classes — light mode */
         buildColorRampCSS()
      );
   }

   /**
    * Build color ramp CSS classes from the 9-color palette.
    * Each ramp has fill (50), stroke (600), and text (800) stops.
    */
   function buildColorRampCSS() {
      var ramps = {
         purple: ['#EEEDFE', '#AFA9EC', '#534AB7', '#3C3489'],
         teal: ['#E1F5EE', '#5DCAA5', '#0F6E56', '#085041'],
         coral: ['#FAECE7', '#F0997B', '#993C1D', '#712B13'],
         pink: ['#FBEAF0', '#ED93B1', '#993556', '#72243E'],
         gray: ['#F1EFE8', '#B4B2A9', '#5F5E5A', '#444441'],
         blue: ['#E6F1FB', '#85B7EB', '#185FA5', '#0C447C'],
         green: ['#EAF3DE', '#97C459', '#3B6D11', '#27500A'],
         amber: ['#FAEEDA', '#EF9F27', '#854F0B', '#633806'],
         red: ['#FCEBEB', '#F09595', '#A32D2D', '#791F1F'],
      };

      var css = '';
      var isDark =
         getComputedStyle(document.documentElement)
            .getPropertyValue('--bg-primary')
            .trim()
            .charAt(1) <= '3'; /* Rough dark mode detection: #1x or #2x */

      Object.keys(ramps).forEach(function (name) {
         var r = ramps[name]; /* [50, 200, 600, 800] */
         if (isDark) {
            /* Dark: 800 fill, 200 stroke, light text */
            css +=
               '.c-' +
               name +
               ' rect, .c-' +
               name +
               ' ellipse, .c-' +
               name +
               ' circle, .c-' +
               name +
               ' polygon, .c-' +
               name +
               ' path:not([fill="none"]) { fill: ' +
               r[3] +
               '; stroke: ' +
               r[1] +
               '; }\n';
            css +=
               '.c-' + name + ' text, .c-' + name + ' .th, .c-' + name + ' .t { fill: #f0f0f0; }\n';
            css += '.c-' + name + ' .ts { fill: #ccc; }\n';
         } else {
            /* Light: 50 fill, 600 stroke, 800 text */
            css +=
               '.c-' +
               name +
               ' rect, .c-' +
               name +
               ' ellipse, .c-' +
               name +
               ' circle, .c-' +
               name +
               ' polygon, .c-' +
               name +
               ' path:not([fill="none"]) { fill: ' +
               r[0] +
               '; stroke: ' +
               r[2] +
               '; }\n';
            css +=
               '.c-' +
               name +
               ' text, .c-' +
               name +
               ' .th, .c-' +
               name +
               ' .t { fill: ' +
               r[3] +
               '; }\n';
            css += '.c-' + name + ' .ts { fill: ' + r[2] + '; }\n';
         }
      });

      return css;
   }

   /**
    * Extract viewBox dimensions from SVG code to compute aspect ratio.
    * Returns { width, height } or null if no viewBox found.
    */
   function parseViewBox(svgCode) {
      var match = svgCode.match(/viewBox\s*=\s*"([^"]*)"/);
      if (!match) return null;

      var parts = match[1].trim().split(/\s+/);
      if (parts.length >= 4) {
         return { width: parseFloat(parts[2]), height: parseFloat(parts[3]) };
      }
      return null;
   }

   /**
    * Create a sandboxed iframe for a visual.
    *
    * Phase 1: Static rendering only. No inline scripts (blocked by CSP).
    * The iframe renders SVG/HTML with theme CSS but without interactivity
    * (onclick/sendPrompt). Height is computed from the SVG viewBox aspect ratio.
    *
    * Uses srcdoc with sandbox="allow-scripts". Parent CSP allows
    * 'unsafe-inline' for script-src to enable the bridge script and
    * onclick handlers. The sandbox attribute provides DOM isolation
    * (no allow-same-origin) — iframe cannot access parent page.
    */
   function createVisualFrame(title, type, code) {
      var container = document.createElement('div');
      container.className = 'dawn-visual-container';
      container.setAttribute('data-visual-title', title);

      var iframe = document.createElement('iframe');
      iframe.sandbox = 'allow-scripts'; /* Scripts allowed for interactivity */
      iframe.title = 'Visual: ' + title;
      iframe.style.cssText = 'width:100%; border:none; overflow:hidden;';

      var themeCSS = buildThemeCSS();
      var visualClasses = buildVisualClasses();

      /* Build the bridge script: sendPrompt + ResizeObserver height reporting */
      var bridgeScript =
         '<script>\n' +
         'function sendPrompt(t){parent.postMessage({type:"dawn_prompt",text:t},"*")}\n' +
         'new ResizeObserver(function(){parent.postMessage(' +
         '{type:"dawn_visual_resize",height:document.body.scrollHeight},"*")})' +
         '.observe(document.body);\n' +
         '</' +
         'script>\n';

      var content;
      if (type === 'svg') {
         content =
            '<!DOCTYPE html><html><head><style>' +
            themeCSS +
            visualClasses +
            'body { margin: 0; padding: 0; overflow: hidden; }\n' +
            'svg { display: block; width: 100%; height: auto; }\n' +
            '</style></head><body>\n' +
            bridgeScript +
            code +
            '</body></html>';

         /* Set initial height from viewBox aspect ratio (ResizeObserver refines it).
          * Use pixel estimate based on 680px design width to avoid vw overshoot. */
         var vb = parseViewBox(code);
         if (vb && vb.width > 0) {
            var aspectRatio = vb.height / vb.width;
            iframe.style.height = Math.min(Math.ceil(aspectRatio * 680), vb.height) + 'px';
         } else {
            iframe.style.height = '400px';
         }
      } else {
         content =
            '<!DOCTYPE html><html><head><style>' +
            themeCSS +
            visualClasses +
            '</style></head><body>\n' +
            bridgeScript +
            code +
            '</body></html>';
         iframe.style.height = '400px';
      }

      iframe.srcdoc = content;
      container.appendChild(iframe);

      /* Register iframe's contentWindow for sendPrompt source validation */
      var frameRef = iframe;
      iframe.addEventListener('load', function () {
         if (frameRef.contentWindow) {
            knownVisualWindows.add(frameRef.contentWindow);
         }
      });

      /* Listen for height updates from ResizeObserver inside the iframe */
      function handleMessage(event) {
         if (!frameRef.contentWindow || event.source !== frameRef.contentWindow) return;
         if (event.data && event.data.type === 'dawn_visual_resize') {
            frameRef.style.height = event.data.height + 'px';
            frameRef.style.maxHeight = ''; /* Clear aspect-ratio cap */
         }
      }
      window.addEventListener('message', handleMessage);

      return container;
   }

   /**
    * Extract <dawn-visual> blocks from raw text BEFORE markdown/sanitize runs.
    * Returns the cleaned text (without visual tags) and an array of visual blocks.
    *
    * @param {string} text - Raw message text that may contain <dawn-visual> tags
    * @returns {{ cleanText: string, visuals: Array<{title: string, type: string, code: string}> }}
    */
   function extractVisuals(text) {
      if (!text || text.indexOf('<dawn-visual') === -1) {
         return { cleanText: text, visuals: [] };
      }

      var visuals = [];
      var match;
      VISUAL_TAG_RE.lastIndex = 0;

      while ((match = VISUAL_TAG_RE.exec(text)) !== null) {
         visuals.push({
            title: match[1],
            type: match[2],
            code: match[3].trim(),
         });
      }

      var cleanText = text.replace(VISUAL_TAG_RE, '').trim();
      return { cleanText: cleanText, visuals: visuals };
   }

   /**
    * Render an array of extracted visual blocks into a text element as iframes.
    *
    * @param {HTMLElement} textEl - The .text element to append visuals to
    * @param {Array<{title: string, type: string, code: string}>} visuals - Extracted visual blocks
    */
   function renderVisuals(textEl, visuals) {
      if (!textEl || !visuals || visuals.length === 0) return;

      visuals.forEach(function (v) {
         var frame = createVisualFrame(v.title, v.type, v.code);
         textEl.appendChild(frame);
      });
   }

   /**
    * Initialize visual renderer.
    * Sets up the sendPrompt bridge listener for interactive diagram nodes.
    */
   function init() {
      window.addEventListener('message', function (event) {
         if (
            event.data &&
            event.data.type === 'dawn_prompt' &&
            typeof event.data.text === 'string' &&
            event.source &&
            knownVisualWindows.has(event.source)
         ) {
            /* Inject prompt text and send as if user typed it */
            if (DawnElements.textInput) {
               DawnElements.textInput.value = event.data.text;
               DawnElements.textInput.dispatchEvent(new Event('input'));
               var enterEvent = new KeyboardEvent('keydown', {
                  key: 'Enter',
                  code: 'Enter',
                  keyCode: 13,
                  which: 13,
                  bubbles: true,
               });
               DawnElements.textInput.dispatchEvent(enterEvent);
            }
         }
      });
   }

   global.DawnVisualRender = {
      init: init,
      extractVisuals: extractVisuals,
      renderVisuals: renderVisuals,
   };
})(window);
