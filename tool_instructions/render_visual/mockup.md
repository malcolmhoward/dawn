# UI mockup guidelines

Use this module for LLM-generated UI mockups — forms, cards, dashboards,
settings panels, login screens, navigation layouts, and any user interface
prototype. Mockups always use HTML type (not SVG), because HTML naturally
handles text reflow, form elements, and responsive layout.

## When to Use Mockup vs. Other Modules

| User intent | Module |
|-------------|--------|
| "What would the settings page look like" | mockup |
| "Design a login form" | mockup |
| "Show me a dashboard layout" | mockup |
| "Chart this data" | chart |
| "How does the auth flow work" | diagram |
| "Build me a working calculator" | interactive |

The key distinction: **mockups show what a UI looks like**, interactive
widgets **actually work**. A mockup of a settings panel has toggle switches
that don't do anything. An interactive widget has toggles that change state.

If the user says "make it work" or "let me try it", use the interactive
module instead. If they say "show me what it would look like" or "mock up
a design", use this module.

## Always Use HTML Type

Mockups must use `type: "html"` in the render_visual call, never `type: "svg"`.
HTML gives you:
- Native form elements (`<input>`, `<select>`, `<textarea>`, `<button>`)
- Flexbox and grid layout
- Text that reflows naturally
- CSS variables for theming
- Proper font rendering

SVG is wrong for mockups because text doesn't wrap, form elements don't
exist, and layout requires manual coordinate math.

## Theming

Use CSS variables from the injected theme for ALL colors. This ensures
mockups match DAWN's current theme (light or dark) automatically.

```css
.card {
   background: var(--color-bg-secondary);
   border: 1px solid var(--color-border);
   border-radius: var(--border-radius-lg);
   padding: 20px;
   color: var(--color-text-primary);
}

.label {
   color: var(--color-text-secondary);
   font-size: 13px;
   font-weight: 500;
   margin-bottom: 4px;
}

.hint {
   color: var(--color-text-tertiary);
   font-size: 12px;
}
```

Never hardcode colors like `#333`, `#fff`, `#f5f5f5`, or `background: white`.
These break in dark mode.

## Layout Patterns

### Single Card
For a focused UI element — a settings panel, a login form, a profile card.

```html
<div style="max-width: 480px; margin: 0 auto;">
   <div style="background: var(--color-bg-secondary);
               border: 1px solid var(--color-border);
               border-radius: var(--border-radius-lg);
               padding: 24px;">
      <h2 style="margin: 0 0 16px; font-size: 18px; font-weight: 500;
                 color: var(--color-text-primary);">Panel title</h2>
      <!-- content -->
   </div>
</div>
```

### Dashboard Grid
For multi-panel layouts — stats overview, admin dashboard, monitoring.

```html
<div style="display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 16px;">
   <div class="card"><!-- card 1 --></div>
   <div class="card"><!-- card 2 --></div>
   <div class="card"><!-- card 3 --></div>
</div>
```

At 680px container width, `minmax(200px, 1fr)` gives you 3 columns.
Use `minmax(300px, 1fr)` for 2 columns. Never force more than 3 columns
at this width — content gets unreadably narrow.

### Sidebar + Content
For settings pages, navigation layouts.

```html
<div style="display: flex; gap: 16px;">
   <nav style="width: 180px; flex-shrink: 0;
               border-right: 1px solid var(--color-border);
               padding-right: 16px;">
      <!-- nav items -->
   </nav>
   <main style="flex: 1; min-width: 0;">
      <!-- content -->
   </main>
</div>
```

### Stacked Sections
For long-form settings or multi-section forms.

```html
<div style="display: flex; flex-direction: column; gap: 24px;">
   <section>
      <h3 style="font-size: 16px; font-weight: 500; margin: 0 0 12px;
                 color: var(--color-text-primary);">Section title</h3>
      <div style="background: var(--color-bg-secondary);
                  border: 1px solid var(--color-border);
                  border-radius: var(--border-radius-lg);
                  padding: 16px;">
         <!-- section content -->
      </div>
   </section>
</div>
```

## Form Elements

Style all form elements to match the DAWN theme. Native browser styling
breaks the illusion of a real UI mockup.

### Text Input

```html
<div style="margin-bottom: 16px;">
   <label style="display: block; font-size: 13px; font-weight: 500;
                 color: var(--color-text-secondary); margin-bottom: 4px;">
      Label
   </label>
   <input type="text" placeholder="Placeholder text"
          style="width: 100%; box-sizing: border-box; padding: 8px 12px;
                 border: 1px solid var(--color-border);
                 border-radius: var(--border-radius-md);
                 background: var(--color-bg-primary);
                 color: var(--color-text-primary);
                 font-family: var(--font-sans); font-size: 14px;
                 outline: none;">
   <p style="margin: 4px 0 0; font-size: 12px;
             color: var(--color-text-tertiary);">
      Helper text or description
   </p>
</div>
```

### Select / Dropdown

```html
<select style="width: 100%; padding: 8px 12px;
               border: 1px solid var(--color-border);
               border-radius: var(--border-radius-md);
               background: var(--color-bg-primary);
               color: var(--color-text-primary);
               font-family: var(--font-sans); font-size: 14px;">
   <option>Option one</option>
   <option>Option two</option>
</select>
```

### Toggle Switch

```html
<label style="display: flex; align-items: center; gap: 8px;
              cursor: pointer; user-select: none;">
   <span style="position: relative; width: 36px; height: 20px;
                background: var(--color-border);
                border-radius: 10px; transition: background 0.2s;">
      <input type="checkbox" checked
             style="position: absolute; opacity: 0;
                    width: 100%; height: 100%; cursor: pointer; margin: 0;">
      <span style="position: absolute; top: 2px; left: 2px;
                   width: 16px; height: 16px; background: white;
                   border-radius: 50%; transition: transform 0.2s;
                   pointer-events: none;"></span>
   </span>
   <span style="font-size: 14px; color: var(--color-text-primary);">
      Setting label
   </span>
</label>
```

Note: The toggle doesn't actually toggle in a static mockup. If you need
working toggles, use the interactive module instead.

### Button

```html
<!-- Primary button -->
<button style="padding: 8px 16px; border: none;
               border-radius: var(--border-radius-md);
               background: var(--color-accent);
               color: white; font-family: var(--font-sans);
               font-size: 14px; font-weight: 500;
               cursor: pointer;">
   Save changes
</button>

<!-- Secondary button -->
<button style="padding: 8px 16px;
               border: 1px solid var(--color-border);
               border-radius: var(--border-radius-md);
               background: var(--color-bg-secondary);
               color: var(--color-text-primary);
               font-family: var(--font-sans);
               font-size: 14px; cursor: pointer;">
   Cancel
</button>
```

### Button Row

```html
<div style="display: flex; justify-content: flex-end; gap: 8px;
            margin-top: 16px; padding-top: 16px;
            border-top: 1px solid var(--color-border);">
   <button><!-- secondary --></button>
   <button><!-- primary --></button>
</div>
```

## Card Patterns

### Stat Card (for dashboards)

```html
<div style="background: var(--color-bg-secondary);
            border: 1px solid var(--color-border);
            border-radius: var(--border-radius-lg);
            padding: 16px;">
   <p style="margin: 0 0 4px; font-size: 13px; font-weight: 500;
             color: var(--color-text-secondary);">
      Active users
   </p>
   <p style="margin: 0; font-size: 28px; font-weight: 500;
             color: var(--color-text-primary);">
      1,247
   </p>
   <p style="margin: 4px 0 0; font-size: 12px;
             color: var(--color-text-tertiary);">
      +12% from last week
   </p>
</div>
```

### List Item (for settings lists, nav items)

```html
<div style="display: flex; align-items: center;
            justify-content: space-between;
            padding: 12px 0;
            border-bottom: 1px solid var(--color-border-light);">
   <div>
      <p style="margin: 0; font-size: 14px;
                color: var(--color-text-primary);">Setting name</p>
      <p style="margin: 2px 0 0; font-size: 12px;
                color: var(--color-text-tertiary);">
         Description of what this controls
      </p>
   </div>
   <!-- toggle, button, or value on the right -->
</div>
```

### Alert / Notice

```html
<!-- Info alert -->
<div style="display: flex; gap: 12px; padding: 12px 16px;
            background: #E6F1FB; border: 1px solid #85B7EB;
            border-radius: var(--border-radius-md);">
   <span style="color: #185FA5; font-weight: 500;">i</span>
   <p style="margin: 0; font-size: 14px; color: #0C447C;">
      Alert message text
   </p>
</div>

<!-- Warning alert: use amber ramp -->
<!-- Error alert: use red ramp -->
<!-- Success alert: use green ramp -->
```

For alerts, use hardcoded hex from the color ramps — they are semantic
(info=blue, warning=amber, error=red, success=green) and should not
change with dark/light mode. Text on alert backgrounds uses the 800 stop
from the same ramp.

## Navigation Patterns

### Tab Bar

```html
<div style="display: flex; gap: 0;
            border-bottom: 2px solid var(--color-border);">
   <button style="padding: 8px 16px; border: none;
                  background: none; font-family: var(--font-sans);
                  font-size: 14px; cursor: pointer;
                  color: var(--color-accent); font-weight: 500;
                  border-bottom: 2px solid var(--color-accent);
                  margin-bottom: -2px;">
      Active tab
   </button>
   <button style="padding: 8px 16px; border: none;
                  background: none; font-family: var(--font-sans);
                  font-size: 14px; cursor: pointer;
                  color: var(--color-text-secondary);">
      Inactive tab
   </button>
</div>
```

### Breadcrumb

```html
<div style="font-size: 13px; color: var(--color-text-tertiary);
            margin-bottom: 16px;">
   <span>Settings</span>
   <span style="margin: 0 6px;">/</span>
   <span>Audio</span>
   <span style="margin: 0 6px;">/</span>
   <span style="color: var(--color-text-primary);">Wake word</span>
</div>
```

## Typography in Mockups

- Page title: 22px, weight 500
- Section heading: 16px, weight 500
- Body text: 14px, weight 400
- Labels: 13px, weight 500, secondary color
- Helper/hint text: 12px, weight 400, tertiary color
- Stat numbers: 28px, weight 500
- All text: `font-family: var(--font-sans)`
- Monospace (code, IDs, paths): `font-family: var(--font-mono)`

## Static vs. Interactive Decision

| Feature | Static mockup | Interactive (use interactive module) |
|---------|--------------|--------------------------------------|
| Toggle switches | Visual only, don't toggle | Actually toggle, change state |
| Form inputs | Accept text but nothing happens | Validate, show errors, submit |
| Tab bars | Show active state, no switching | Click to switch content |
| Dropdowns | Show options, no selection | Select and update view |
| Buttons | Visual only | onClick triggers actions |

If you need more than 2-3 interactive elements, switch to the interactive
module. Mockups with too many non-functional controls frustrate users.

## Common Failure Modes

1. **Hardcoded colors:** Using `#333`, `white`, `#f5f5f5` instead of CSS
   variables. Breaks in dark mode. Always use `var(--color-*)`.
2. **Missing box-sizing:** Inputs and textareas overflow their containers
   without `box-sizing: border-box`.
3. **Too many columns:** More than 3 columns at 680px width makes content
   unreadable. Use `minmax(200px, 1fr)` in grid.
4. **Unstyled form elements:** Browser-default checkboxes, selects, and
   inputs break the mockup illusion. Style everything.
5. **No font-family:** Forgetting `font-family: var(--font-sans)` on
   buttons and inputs — they inherit browser defaults (often serif).
6. **Alert colors in dark mode:** Using hardcoded light-theme alert
   backgrounds that wash out in dark mode. Use the ramp 50 stop for
   light mode and ramp 800 stop for dark mode backgrounds.
7. **Pixel-perfect obsession:** Mockups should communicate layout and
   hierarchy, not be production CSS. Don't waste tokens on 1px alignment
   tweaks. Get the structure right, not the polish.
