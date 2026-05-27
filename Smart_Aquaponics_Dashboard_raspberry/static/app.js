const ids = [
  'ph', 'ec_us', 'air', 'water_level', 'flow', 'box_temp_c', 'humidity',
  'water_temp_c', 'light', 'fan', 'light1', 'light2', 'light3', 'solenoid',
  'pump', 'feeder', 'updated_at'
];

function fmt(value, digits = 1) {
  if (value === null || value === undefined) return '--';
  if (typeof value === 'number') return value.toFixed(digits);
  return value;
}

async function refresh() {
  try {
    const res = await fetch('/api/data', { cache: 'no-store' });
    const data = await res.json();
    for (const id of ids) {
      const el = document.getElementById(id);
      if (!el) continue;
      if (id === 'ph') el.textContent = fmt(data[id], 2);
      else if (id === 'ec_us') el.textContent = fmt(data[id], 0);
      else if (id.endsWith('_temp_c') || id === 'humidity') el.textContent = fmt(data[id], 1);
      else el.textContent = fmt(data[id]);
    }
    document.getElementById('lux').textContent = data.lux == null ? '-- lux' : `${Number(data.lux).toFixed(1)} lux`;
    const conn = document.getElementById('connection');
    conn.textContent = data.status || 'unknown';
    conn.className = `status-pill ${data.status || ''}`;
    document.getElementById('error').textContent = data.error || '';
  } catch (error) {
    document.getElementById('connection').textContent = 'offline';
    document.getElementById('connection').className = 'status-pill disconnected';
    document.getElementById('error').textContent = error.message;
  }
}

async function post(url, payload) {
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
  const data = await res.json();
  if (!data.ok) alert(data.error || 'Command failed');
  setTimeout(refresh, 400);
}

function cmd(action) {
  post('/api/command', { action });
}

function scheduleLight(light) {
  const onHours = Number(document.getElementById('lightOnHours').value || 0);
  const offHours = Number(document.getElementById('lightOffHours').value || 0);
  post('/api/light_schedule', {
    light,
    on_seconds: Math.round(onHours * 3600),
    off_seconds: Math.round(offHours * 3600),
  });
}

function scheduleFeed() {
  const intervalHours = Number(document.getElementById('feedIntervalHours').value || 0);
  const durationMs = Number(document.getElementById('feedDurationMs').value || 0);
  post('/api/feed_schedule', {
    interval_seconds: Math.round(intervalHours * 3600),
    duration_ms: Math.round(durationMs),
  });
}

refresh();
setInterval(refresh, 1500);
