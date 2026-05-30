const ids = [
  'ph', 'ec_us', 'air', 'water_level', 'flow', 'box_temp_c', 'humidity',
  'water_temp_c', 'light', 'fan', 'light1', 'light2', 'light3', 'solenoid',
  'pump', 'feeder', 'updated_at'
];

let previousData = null;
let notificationsEnabled = localStorage.getItem('notificationsEnabled') === 'true';
let previousAlerts = {};

const watchedFields = {
  status: 'Dashboard connection',
  air: 'Air quality',
  water_level: 'Water level',
  flow: 'Water flow',
  light: 'Light level',
  fan: 'Fan',
  light1: 'Light 1',
  light2: 'Light 2',
  light3: 'Light 3',
  solenoid: 'Solenoid valve',
  pump: 'Pump',
  feeder: 'Fish feeder',
};

const sensorRanges = {
  ph: { label: 'pH', min: 6.5, max: 7.5, unit: '', digits: 2 },
  ec_us: { label: 'EC', min: 500, max: 2000, unit: ' uS/cm', digits: 0 },
  box_temp_c: { label: 'Box temperature', min: 18, max: 32, unit: ' C', digits: 1 },
  humidity: { label: 'Humidity', min: 50, max: 85, unit: '%', digits: 1 },
  water_temp_c: { label: 'Water temperature', min: 20, max: 30, unit: ' C', digits: 1 },
  lux: { label: 'Grow light', min: 1000, max: 70000, unit: ' lux', digits: 0 },
};

const criticalStates = {
  air: { BAD: 'Air quality is bad' },
  water_level: { LOW: 'Water level is low' },
  flow: { NO: 'No water flow detected' },
  status: { disconnected: 'Dashboard lost Arduino connection' },
};

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
    checkStateChanges(data);
    checkSensorAlerts(data);
  } catch (error) {
    document.getElementById('connection').textContent = 'offline';
    document.getElementById('connection').className = 'status-pill disconnected';
    document.getElementById('error').textContent = error.message;
  }
}

function formatRangeValue(value, range) {
  return `${Number(value).toFixed(range.digits)}${range.unit}`;
}

function getSensorAlert(field, value) {
  const range = sensorRanges[field];
  if (!range) return null;
  if (value === null || value === undefined || Number.isNaN(Number(value))) {
    return `${range.label} is not reading`;
  }

  const numberValue = Number(value);
  if (numberValue < range.min) {
    return `${range.label} low: ${formatRangeValue(numberValue, range)}. Target ${range.min}-${range.max}${range.unit}`;
  }
  if (numberValue > range.max) {
    return `${range.label} high: ${formatRangeValue(numberValue, range)}. Target ${range.min}-${range.max}${range.unit}`;
  }
  return null;
}

function getStateAlert(field, value, data) {
  const states = criticalStates[field];
  if (!states || value === null || value === undefined) return null;
  if (field === 'flow' && data.pump !== 'ON') return null;
  return states[value] || null;
}

function checkSensorAlerts(data) {
  const alerts = {};

  for (const field of Object.keys(sensorRanges)) {
    const alert = getSensorAlert(field, data[field]);
    if (alert) alerts[field] = alert;
  }

  for (const field of Object.keys(criticalStates)) {
    const alert = getStateAlert(field, data[field], data);
    if (alert) alerts[field] = alert;
  }

  for (const [field, alert] of Object.entries(alerts)) {
    if (previousAlerts[field] !== alert) sendNotification('Sensor alert', alert);
  }

  for (const field of Object.keys(previousAlerts)) {
    if (!alerts[field]) {
      const label = sensorRanges[field]?.label || watchedFields[field] || field;
      sendNotification('Sensor normal', `${label} back in normal range`);
    }
  }

  previousAlerts = alerts;
}

function setNotifyStatus() {
  const el = document.getElementById('notifyStatus');
  if (!el) return;
  if (!('Notification' in window)) {
    el.textContent = 'Not supported by this browser';
  } else if (Notification.permission === 'granted' && notificationsEnabled) {
    el.textContent = 'Enabled';
  } else if (Notification.permission === 'denied') {
    el.textContent = 'Blocked in browser settings';
  } else {
    el.textContent = 'Not enabled';
  }
}

async function enableNotifications() {
  notificationsEnabled = true;
  localStorage.setItem('notificationsEnabled', 'true');

  if ('Notification' in window && Notification.permission !== 'granted') {
    await Notification.requestPermission();
  }

  setNotifyStatus();
  sendNotification('Smart Hydro Aquaponic', 'Notifications enabled');
}

function sendNotification(title, body) {
  if (navigator.vibrate) navigator.vibrate([150, 80, 150]);

  if ('Notification' in window && Notification.permission === 'granted' && notificationsEnabled) {
    new Notification(title, { body });
  }

  showToast(`${title}: ${body}`);
}

function showToast(message) {
  let toast = document.getElementById('toast');
  if (!toast) {
    toast = document.createElement('div');
    toast.id = 'toast';
    toast.style.cssText = 'position:fixed;left:16px;right:16px;bottom:18px;background:#5cf2b7;color:#062018;padding:14px 16px;border-radius:16px;font-weight:900;z-index:9999;box-shadow:0 10px 30px rgba(0,0,0,.35);';
    document.body.appendChild(toast);
  }
  toast.textContent = message;
  toast.hidden = false;
  clearTimeout(showToast.timer);
  showToast.timer = setTimeout(() => { toast.hidden = true; }, 4500);
}

function checkStateChanges(data) {
  if (!previousData) {
    previousData = { ...data };
    return;
  }

  for (const [field, label] of Object.entries(watchedFields)) {
    if (data[field] === undefined || previousData[field] === undefined) continue;
    if (data[field] !== previousData[field]) {
      sendNotification(label, `${previousData[field]} → ${data[field]}`);
    }
  }

  previousData = { ...data };
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
setNotifyStatus();
setInterval(refresh, 1500);
