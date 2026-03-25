# Diagram guidelines

## Diagram Types

Pick the right type based on INTENT, not subject matter.

### Flowchart
For sequential processes, cause-and-effect, decision trees.
- Trigger: "walk me through", "what are the steps", "what's the flow"
- Layout: single-direction flows (top-down or left-right)
- Max 4-5 nodes per diagram
- 60px minimum vertical spacing between boxes
- 24px padding inside boxes, 12px between text and edges

### Structural Diagram
For containment — things inside other things.
- Trigger: "what's the architecture", "how is this organized"
- Large rounded rects (rx=20) as containers, smaller rects inside
- 20px minimum padding inside every container
- Max 2-3 nesting levels at 680px width
- Use different color ramps for nested levels

### Illustrative Diagram
For building INTUITION. Draw the mechanism, not a diagram about it.
- Trigger: "how does X actually work", "explain X", "I don't get X"
- Physical subjects: simplified cross-sections, cutaways
- Abstract subjects: spatial metaphors (hash table = row of buckets,
  attention = fan of lines with varying thickness)
- Color encodes intensity (warm = active, cool = dormant)
- Shapes are freeform — use path, ellipse, circle, polygon
- One gradient allowed per diagram, only for continuous physical
  properties (temperature, pressure)
- Prefer interactive over static when the subject has a control

## Layout Rules

### Box Sizing
- Single-line box: 44px tall, title only
- Two-line box: 56px tall, title + subtitle
- Width: max(title_chars x 8, subtitle_chars x 7) + 24px minimum

### Spacing
- 60px minimum between connected boxes vertically
- 20px minimum gap between boxes in the same row
- 10px gap between arrowhead and box edge
- Two-line boxes: 22px between title and subtitle baselines

### Tier Packing (horizontal rows)
Compute total width BEFORE placing boxes:
- 4 boxes x 130px + 3 gaps x 20px = 580px (fits in 600px safe area)
- 4 boxes x 160px + 3 gaps x 20px = 700px (OVERFLOW — shrink boxes)

### Arrow Routing
- A line from A to B must NOT cross any other box or label.
- If direct path crosses something, route around with L-bend:
  `<path d="M x1 y1 L x1 ymid L x2 ymid L x2 y2" fill="none"/>`
- No standalone arrow labels — if meaning isn't obvious from source
  and target, put it in the box subtitle or prose.

### ViewBox Height
After layout, find max_y (bottom-most element including text baselines).
Set viewBox height = max_y + 40px buffer. Don't guess.

## Common Failure Modes
Check each before finalizing:

1. **Text overflow:** label wider than its box. Always compute width first.
2. **Arrow through box:** line crosses an unrelated node. Route around it.
3. **ViewBox too short:** bottom content clipped. Compute height from actual elements.
4. **Dark mode invisible:** hardcoded text color on transparent background.
5. **Overlapping boxes:** didn't compute tier packing before placing.
6. **text-anchor="end" near x=0:** text extends left past viewBox boundary.

## Flowchart Component Patterns

Single-line node (44px):
```xml
<g class="node c-blue" onclick="sendPrompt('Tell me about X')">
  <rect x="100" y="20" width="180" height="44" rx="8" stroke-width="0.5"/>
  <text class="th" x="190" y="42" text-anchor="middle"
        dominant-baseline="central">Label</text>
</g>
```

Two-line node (56px):
```xml
<g class="node c-blue" onclick="sendPrompt('Tell me about X')">
  <rect x="100" y="20" width="200" height="56" rx="8" stroke-width="0.5"/>
  <text class="th" x="200" y="38" text-anchor="middle"
        dominant-baseline="central">Title</text>
  <text class="ts" x="200" y="56" text-anchor="middle"
        dominant-baseline="central">Short subtitle</text>
</g>
```

Connector (no label):
```xml
<line x1="200" y1="76" x2="200" y2="120" class="arr"
      marker-end="url(#arrow)"/>
```

## Illustrative Diagram Specifics

### What Changes From Flowcharts
- Shapes are freeform: `<path>`, `<ellipse>`, `<circle>`, `<polygon>`
- Layout follows the subject's geometry, not a grid
- Color encodes intensity, not category
- Layering and overlap are encouraged for shapes (NOT for text)
- Small shape indicators allowed: triangles for flames, circles for
  bubbles, wavy lines for steam

### Label Placement
- Place labels OUTSIDE the drawn object with leader lines when possible
- Leader lines: 0.5px dashed, `var(--color-text-tertiary)` stroke
- Pick ONE side for labels — no room for both at 680px
- Reserve 140px+ of margin on the label side
- Default to right-side labels with `text-anchor="start"`

### Composition Order
1. Main object silhouette (largest shape, centered)
2. Internal structure (chambers, pipes, components)
3. External connections (arrows, inputs, outputs)
4. State indicators last (color fills, animation, small markers)

### Physical Color Scenes
For scenes with natural colors (sky, water, grass, materials):
use ALL hardcoded hex — never mix with `c-*` theme classes.
The scene should not invert in dark mode.

## Multi-Diagram Responses
For complex topics, use multiple render_visual calls with prose between.
Never stack two calls back-to-back without text in between.

## Cycles and Loops
DO NOT draw cycles as rings — stages around a circle always cause
collisions. Instead:
- Linear layout with a curved return arrow, OR
- HTML stepper (one panel per stage, Next wraps to first)
