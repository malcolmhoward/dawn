# Chart guidelines

Use Chart.js for data visualization. Load from CDN inside HTML type visuals.

## Setup
```html
<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.js"></script>

<canvas id="myChart" style="max-height:400px"></canvas>
<script>
const ctx = document.getElementById('myChart');
new Chart(ctx, {
   type: 'bar',
   data: { ... },
   options: {
      responsive: true,
      plugins: { legend: { position: 'top' } }
   }
});
</script>
```

## Theming
Read CSS variables and apply to Chart.js:
```javascript
const style = getComputedStyle(document.documentElement);
const textColor = style.getPropertyValue('--color-text-primary').trim();
const gridColor = style.getPropertyValue('--color-border').trim();

Chart.defaults.color = textColor;
Chart.defaults.borderColor = gridColor;
```

## Color Mapping for Datasets
Use color ramp 600 stops for dataset colors:
- Dataset 1: #534AB7 (purple-600)
- Dataset 2: #0F6E56 (teal-600)
- Dataset 3: #D85A30 (coral-600)
- Dataset 4: #185FA5 (blue-600)

## Chart Type Selection
- Comparison across categories → bar (horizontal if long labels)
- Trend over time → line
- Part of whole → doughnut (not pie — doughnut is more readable)
- Correlation → scatter
- Multi-variable comparison → radar
