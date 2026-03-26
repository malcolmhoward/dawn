# Art and illustration guidelines

Use this module for LLM-generated SVG illustrations, decorative visuals,
generative patterns, and scenes. This covers output where the goal is
aesthetic or atmospheric rather than informational.

## When to Use Art vs. Diagram

| User intent | Module |
|-------------|--------|
| "How does X work" | diagram (illustrative type) |
| "Draw me a sunset" | art |
| "Make something decorative for my dashboard" | art |
| "Show the architecture" | diagram (structural type) |
| "Create a pattern" | art |
| "Visualize this data" | chart |
| "Explain X visually" | diagram |

The key distinction: **diagrams explain mechanisms**, art creates **scenes,
objects, patterns, and atmosphere**. If the visual needs labels and arrows,
it's a diagram. If it needs composition and color harmony, it's art.

Note: For photorealistic or complex image generation, use the
`image_generate` tool (stable-diffusion.cpp) instead. SVG art is best for
geometric, stylized, or pattern-based visuals — not photorealism.

## Color Rules for Art

### Themed/Abstract Art (patterns, geometric, decorative)
Use the color ramp hex values directly from `_core.md`. These adapt to
dark mode automatically when applied via `c-{ramp}` classes.

Pick 2-3 ramps that harmonize:
- Purple + teal: cool, sophisticated
- Coral + amber: warm, energetic
- Blue + green: natural, calm
- Pink + purple: vibrant, playful

### Physical/Scene Art (sky, water, landscapes, objects)
Use ALL hardcoded hex values. Scenes with natural colors must NOT invert
in dark mode — a blue sky should stay blue regardless of theme.

Do NOT mix `c-{ramp}` classes with hardcoded scene colors in the same
SVG. The scene will half-invert in dark mode and look broken.

If you need a dark-mode variant of a scene, provide it explicitly:
```css
@media (prefers-color-scheme: dark) {
   .sky { fill: #1a1a3e; }    /* night sky */
   .water { fill: #0a2a4a; }  /* dark water */
}
```
This is the one place `@media (prefers-color-scheme)` is allowed in SVG.

## Shape Variety

SVG art should use the full shape vocabulary, not just rects:

- `<circle>` and `<ellipse>` — organic forms, dots, celestial bodies
- `<polygon>` — crystals, geometric patterns, angular forms
- `<path>` — freeform curves, silhouettes, terrain, organic edges
- `<line>` and `<polyline>` — rays, grids, structural lines
- `<rect>` with large `rx` — pills, rounded tiles, soft shapes

### Path Complexity Ceiling
Keep paths recognizable at a glance. If a `<path>` needs more than ~8-10
segments (M/L/C/Q commands) to draw, simplify it. A tree is a triangle on
a rectangle, not a detailed botanical illustration. A mountain is a jagged
polyline, not a contour map. Recognizable silhouette beats accurate outline.

### Freeform Curves
Use cubic Bezier (`C`) for smooth organic curves:
```xml
<path d="M100 300 C150 200, 250 200, 300 300" fill="none"
      stroke="#0F6E56" stroke-width="2"/>
```
Use quadratic Bezier (`Q`) for simpler arcs:
```xml
<path d="M100 300 Q200 150 300 300" fill="#E1F5EE"/>
```

## Composition Rules

### Layout
- Center the main subject in the viewBox.
- Leave 40px+ margin on all sides for breathing room.
- Layer elements back-to-front (later in source = on top).
- Background elements: large, muted, low opacity.
- Foreground elements: smaller, vivid, full opacity.

### Balance
- Visual weight should feel balanced left-to-right.
- If the main subject is off-center, balance with smaller elements
  on the opposite side.
- Odd numbers of repeated elements (3, 5, 7) look more natural than even.

### Depth
Create depth through:
- **Size:** larger = closer, smaller = farther
- **Opacity:** lower opacity = farther / atmospheric perspective
- **Vertical position:** lower in frame = closer (ground plane)
- **Overlap:** closer objects overlap farther ones
- **Color saturation:** vivid = close, muted = distant

## Gradients in Art

Art is the one module where gradients are used more freely than in diagrams,
but they still have rules:

- `<linearGradient>` only. No radial gradients (they render inconsistently
  across SVG implementations).
- Maximum 3 gradients per SVG. More creates visual noise.
- Use gradients for: skies (horizon to zenith), water (surface to depth),
  terrain (lit to shadow), atmospheric effects.
- Keep to 2-3 stops per gradient. More stops rarely improve the result.
- Define in `<defs>`, reference via `fill="url(#gradientId)"`.

```xml
<defs>
  <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0%" stop-color="#1a1a3e"/>
    <stop offset="100%" stop-color="#d4577a"/>
  </linearGradient>
</defs>
<rect x="0" y="0" width="680" height="300" fill="url(#sky)"/>
```

## Pattern Generation

For geometric patterns (tiles, tessellations, backgrounds):

- Use `<pattern>` in `<defs>` for repeating elements.
- Keep the pattern unit simple — 1-3 shapes maximum.
- Pattern units should tile seamlessly (edges align).
- Use transform="rotate()" or transform="translate()" for variation.

```xml
<defs>
  <pattern id="dots" x="0" y="0" width="20" height="20"
           patternUnits="userSpaceOnUse">
    <circle cx="10" cy="10" r="3" fill="#534AB7" opacity="0.3"/>
  </pattern>
</defs>
<rect width="680" height="400" fill="url(#dots)"/>
```

## Animation in Art

CSS animations are allowed for art (same rules as interactive module):
- `@keyframes` only, animate `transform` and `opacity`.
- Wrap in `@media (prefers-reduced-motion: no-preference)`.
- Keep loops smooth and subtle — art animations should feel ambient,
  not distracting.

Good animation subjects: twinkling stars, drifting clouds, gentle waves,
floating particles, pulsing glow.

```css
@media (prefers-reduced-motion: no-preference) {
  @keyframes twinkle {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.3; }
  }
  .star { animation: twinkle 3s ease-in-out infinite; }
}
```

Stagger animation delays across repeated elements for natural feel:
```xml
<circle class="star" cx="100" cy="50" r="2" style="animation-delay:0s"/>
<circle class="star" cx="300" cy="80" r="1.5" style="animation-delay:1.2s"/>
<circle class="star" cx="500" cy="30" r="2.5" style="animation-delay:0.6s"/>
```

## Common Failure Modes

1. **Photorealism attempt:** SVG cannot do photorealism. Keep everything
   stylized, geometric, or silhouette-based. If the user wants a realistic
   image, suggest `image_generate` instead.
2. **Too many path segments:** Complex `<path>` elements that try to trace
   detailed outlines. Simplify to recognizable silhouettes.
3. **Dark mode scene inversion:** Scene with hardcoded sky blue and
   `c-{ramp}` foreground elements — half inverts, half doesn't. Use ALL
   hardcoded or ALL themed, never mix.
4. **Gradient soup:** More than 3 gradients makes the SVG feel muddy.
   Flat fills with opacity variation often look better.
5. **Overloaded composition:** Too many elements competing for attention.
   One focal point, supporting elements subdued.
6. **Tiny details at 680px:** Fine details smaller than ~4px will be
   invisible or aliased. Keep the smallest meaningful element at 4px+.
