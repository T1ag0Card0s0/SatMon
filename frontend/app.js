const REMOTE_API_ENDPOINT = "https://n06cy09ved.execute-api.eu-west-1.amazonaws.com/dev/SatMonHTTP";
const DEFAULT_API_ENDPOINT = window.location.origin === "null" ? REMOTE_API_ENDPOINT : `${window.location.origin}/api`;
const DEFAULT_LED_COLOR = "#14c3b7";
const LED_TARGET_SELECTED = "selected";
const LED_TARGET_ALL = "all";
const REFRESH_INTERVAL_MS = 1000;
const WARNING_TEMPERATURE = 45;
const CRITICAL_TEMPERATURE = 55;
const STALE_PACKET_SECONDS = 5;
const DISCONNECTED_PACKET_SECONDS = 15;
const MPU_GYRO_UDPS_PER_LSB = 7634;
const LSM6DS_GYRO_UDPS_PER_LSB = 8750;
const MAX_GYRO_STEP_SECONDS = 2;
const ATTITUDE_ACCEL_BLEND = 0.35;

const savedApiEndpoint = localStorage.getItem("satmon.apiEndpoint");
const savedEndpointUsesRemoteApi = savedApiEndpoint?.startsWith(REMOTE_API_ENDPOINT);

const state = {
  endpoint: savedApiEndpoint && !savedEndpointUsesRemoteApi ? savedApiEndpoint : DEFAULT_API_ENDPOINT,
  satellites: [],
  telemetryBySatellite: new Map(),
  selectedSatelliteId: localStorage.getItem("satmon.selectedSatellite") || "",
  packetLimit: Number(localStorage.getItem("satmon.packetLimit") || 25),
  autoRefresh: localStorage.getItem("satmon.autoRefresh") !== "false",
  ledEnabled: localStorage.getItem("satmon.ledEnabled") === "true",
  ledTarget: localStorage.getItem("satmon.ledTarget") || LED_TARGET_SELECTED,
  ledColor: localStorage.getItem("satmon.ledColor") || DEFAULT_LED_COLOR,
  loading: false,
  ledLoading: false,
  refreshTimer: null,
};

const els = {
  apiState: document.querySelector("#apiState"),
  healthStatus: document.querySelector("#healthStatus"),
  lastUpdate: document.querySelector("#lastUpdate"),
  notice: document.querySelector("#notice"),
  activeSatellites: document.querySelector("#activeSatellites"),
  activeAlerts: document.querySelector("#activeAlerts"),
  averageTemperature: document.querySelector("#averageTemperature"),
  packetCount: document.querySelector("#packetCount"),
  satelliteGrid: document.querySelector("#satelliteGrid"),
  alertList: document.querySelector("#alertList"),
  historyChart: document.querySelector("#historyChart"),
  controlForm: document.querySelector("#controlForm"),
  apiEndpoint: document.querySelector("#apiEndpoint"),
  selectedSatelliteLabel: document.querySelector("#selectedSatelliteLabel"),
  packetLimit: document.querySelector("#packetLimit"),
  autoRefresh: document.querySelector("#autoRefresh"),
  ledForm: document.querySelector("#ledForm"),
  ledEnabled: document.querySelector("#ledEnabled"),
  ledTarget: document.querySelector("#ledTarget"),
  ledColor: document.querySelector("#ledColor"),
  ledPreview: document.querySelector("#ledPreview"),
  sendLedButton: document.querySelector("#sendLedButton"),
  ledStatus: document.querySelector("#ledStatus"),
  packetTable: document.querySelector("#packetTable"),
  attitudeCanvas: document.querySelector("#attitudeCanvas"),
  attitudeEmpty: document.querySelector("#attitudeEmpty"),
  attitudeSatellite: document.querySelector("#attitudeSatellite"),
  attitudeRoll: document.querySelector("#attitudeRoll"),
  attitudePitch: document.querySelector("#attitudePitch"),
  attitudeYaw: document.querySelector("#attitudeYaw"),
  attitudeGyro: document.querySelector("#attitudeGyro"),
};

let attitudeView = null;

function normalizeEndpoint(value) {
  return value.trim().replace(/\/+$/, "");
}

function apiUrl(path) {
  return `${state.endpoint}${path}`;
}

function cssColor(name, fallback) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;
}

function refreshIcons() {
  if (window.lucide) {
    window.lucide.createIcons();
  }
}

function setIconButton(button, icon, label) {
  const iconNode = document.createElement("i");
  const labelNode = document.createElement("span");

  iconNode.setAttribute("data-lucide", icon);
  iconNode.setAttribute("aria-hidden", "true");
  labelNode.textContent = label;
  button.replaceChildren(iconNode, labelNode);
  refreshIcons();
}

function saveSettings() {
  localStorage.setItem("satmon.apiEndpoint", state.endpoint);
  localStorage.setItem("satmon.selectedSatellite", state.selectedSatelliteId);
  localStorage.setItem("satmon.packetLimit", String(state.packetLimit));
  localStorage.setItem("satmon.autoRefresh", String(state.autoRefresh));
  localStorage.setItem("satmon.ledEnabled", String(state.ledEnabled));
  localStorage.setItem("satmon.ledTarget", state.ledTarget);
  localStorage.setItem("satmon.ledColor", state.ledColor);
}

function formatClock(value) {
  const date = value ? new Date(value) : new Date();

  if (Number.isNaN(date.getTime())) {
    return "--:--:--";
  }

  return date.toLocaleTimeString([], {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

function formatTimestamp(value) {
  const date = value ? new Date(value) : null;

  if (!date || Number.isNaN(date.getTime())) {
    return value || "--";
  }

  return date.toISOString().replace("T", " ").replace(".000Z", "Z");
}

function secondsSince(value) {
  const date = new Date(value);

  if (Number.isNaN(date.getTime())) {
    return Infinity;
  }

  return Math.max(0, (Date.now() - date.getTime()) / 1000);
}

function relativeAgeText(value) {
  const ageSeconds = secondsSince(value);

  if (!Number.isFinite(ageSeconds)) {
    return "Never";
  }

  if (ageSeconds < 5) {
    return "Just now";
  }

  if (ageSeconds < 60) {
    return `${Math.floor(ageSeconds)}s ago`;
  }

  if (ageSeconds < 3600) {
    return `${Math.floor(ageSeconds / 60)}m ago`;
  }

  if (ageSeconds < 86400) {
    return `${Math.floor(ageSeconds / 3600)}h ago`;
  }

  return `${Math.floor(ageSeconds / 86400)}d ago`;
}

function normalizeTemperature(value) {
  if (value === undefined || value === null || value === "") {
    return null;
  }

  const numeric = Number(value);

  if (!Number.isFinite(numeric)) {
    return null;
  }

  // Telemetry temperature is transmitted in tenths of a degree Celsius (deci-C).
  return numeric / 10;
}

function formatTemperature(value) {
  const temperature = normalizeTemperature(value);

  if (temperature === null) {
    return "--";
  }

  return `${temperature.toFixed(1)} C`;
}

function vectorValue(vector, key) {
  if (!vector || typeof vector !== "object" || vector[key] === undefined) {
    return "--";
  }

  return String(vector[key]);
}

function compactVector(vector) {
  if (!vector || typeof vector !== "object") {
    return "--";
  }

  return `${vectorValue(vector, "x")}, ${vectorValue(vector, "y")}, ${vectorValue(vector, "z")}`;
}

function numericValue(value) {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric : null;
}

function numericVector(vector) {
  if (!vector || typeof vector !== "object") {
    return null;
  }

  const x = numericValue(vector.x);
  const y = numericValue(vector.y);
  const z = numericValue(vector.z);

  if (x === null || y === null || z === null) {
    return null;
  }

  return { x, y, z };
}

function degToRad(value) {
  return (value * Math.PI) / 180;
}

function radToDeg(value) {
  return (value * 180) / Math.PI;
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function wrapRadians(value) {
  const fullTurn = Math.PI * 2;
  return ((((value + Math.PI) % fullTurn) + fullTurn) % fullTurn) - Math.PI;
}

function blendAngle(current, target, weight) {
  return wrapRadians(current + wrapRadians(target - current) * weight);
}

function formatDegrees(value) {
  return Number.isFinite(value) ? `${value.toFixed(1)} deg` : "--";
}

function gyroVectorForPacket(packet) {
  return packet?.sensors?.mpu?.gyroscope || packet?.sensors?.gyroscope || packet?.gyroscope || null;
}

function imuMetadataForPacket(packet) {
  return packet?.sensors?.mpu || packet?.imu || packet?.sensors || {};
}

function accelerometerVectorForPacket(packet) {
  return packet?.sensors?.mpu?.accelerometer || packet?.sensors?.accelerometer || packet?.accelerometer || null;
}

function gyroMicroDegreesPerSecondPerLsb(packet) {
  const metadata = imuMetadataForPacket(packet);
  const configuredScale = numericValue(metadata.gyroscope_udps_per_lsb || metadata.gyro_udps_per_lsb);

  if (configuredScale !== null && configuredScale > 0) {
    return configuredScale;
  }

  const model = String(metadata.model || metadata.type || "").toLowerCase();

  if (model.includes("lsm6")) {
    return LSM6DS_GYRO_UDPS_PER_LSB;
  }

  return MPU_GYRO_UDPS_PER_LSB;
}

function gyroDegreesPerSecond(vector, packet = null) {
  const numeric = numericVector(vector);

  if (!numeric) {
    return null;
  }

  const metadata = imuMetadataForPacket(packet);
  const units = String(metadata.gyroscope_units || metadata.gyro_units || "").toLowerCase();
  const scale = units.includes("dps") || units.includes("deg/s")
    ? 1
    : gyroMicroDegreesPerSecondPerLsb(packet) / 1000000;

  return {
    x: numeric.x * scale,
    y: numeric.y * scale,
    z: numeric.z * scale,
  };
}

function formatGyroVector(vector) {
  if (!vector) {
    return "--";
  }

  return `x ${vector.x.toFixed(2)} dps / y ${vector.y.toFixed(2)} dps / z ${vector.z.toFixed(2)} dps`;
}

function packetTimeMs(packet) {
  const date = new Date(packet?.timestamp);
  return Number.isNaN(date.getTime()) ? null : date.getTime();
}

function candidateAttitudeObjects(packet) {
  return [
    packet?.orientation,
    packet?.attitude,
    packet?.sensors?.orientation,
    packet?.sensors?.attitude,
    packet?.sensors?.mpu?.orientation,
    packet?.sensors?.mpu?.attitude,
  ].filter(Boolean);
}

function angleValue(source, keys) {
  for (const key of keys) {
    const value = numericValue(source?.[key]);

    if (value !== null) {
      return Math.abs(value) > Math.PI * 2 ? degToRad(value) : value;
    }
  }

  return null;
}

function explicitAttitudeForPacket(packet) {
  for (const source of candidateAttitudeObjects(packet)) {
    const roll = angleValue(source, ["roll", "r", "phi", "x"]);
    const pitch = angleValue(source, ["pitch", "p", "theta", "y"]);
    const yaw = angleValue(source, ["yaw", "heading", "psi", "z"]);

    if (roll !== null || pitch !== null || yaw !== null) {
      return {
        roll: roll || 0,
        pitch: pitch || 0,
        yaw: yaw || 0,
      };
    }
  }

  return null;
}

function explicitQuaternionForPacket(packet) {
  for (const source of candidateAttitudeObjects(packet)) {
    const quaternion = source.quaternion || source;
    const x = numericValue(quaternion?.x);
    const y = numericValue(quaternion?.y);
    const z = numericValue(quaternion?.z);
    const w = numericValue(quaternion?.w);

    if (x !== null && y !== null && z !== null && w !== null) {
      return { x, y, z, w };
    }
  }

  return null;
}

function accelerometerTilt(vector) {
  const accelerometer = numericVector(vector);

  if (!accelerometer) {
    return null;
  }

  const magnitude = Math.hypot(accelerometer.x, accelerometer.y, accelerometer.z);

  if (magnitude < 0.001) {
    return null;
  }

  return {
    roll: Math.atan2(accelerometer.y, accelerometer.z),
    pitch: Math.atan2(-accelerometer.x, Math.hypot(accelerometer.y, accelerometer.z)),
  };
}

function attitudeQuaternionFromAngles(attitude) {
  const THREE = window.THREE;
  const quaternion = new THREE.Quaternion();
  quaternion.setFromEuler(new THREE.Euler(attitude.pitch, attitude.yaw, attitude.roll, "YXZ"));
  return quaternion;
}

function attitudeFromQuaternion(quaternion) {
  const THREE = window.THREE;
  const euler = new THREE.Euler().setFromQuaternion(quaternion, "YXZ");

  return {
    roll: euler.z,
    pitch: euler.x,
    yaw: euler.y,
  };
}

function deriveAttitude(packets) {
  const selectedPackets = packets.filter(Boolean).slice(0, state.packetLimit);
  const latestPacket = selectedPackets[0] || null;
  const latestGyro = gyroDegreesPerSecond(gyroVectorForPacket(latestPacket), latestPacket);
  const latestExplicit = explicitAttitudeForPacket(latestPacket);

  if (latestExplicit) {
    const quaternion = window.THREE ? attitudeQuaternionFromAngles(latestExplicit) : null;
    return {
      ...latestExplicit,
      gyro: latestGyro,
      quaternion,
      source: "telemetry",
    };
  }

  const chronologicalPackets = [...selectedPackets].reverse();
  let lastTimeMs = null;
  let attitude = null;
  let hasAttitudeData = false;

  chronologicalPackets.forEach((packet) => {
    const packetMs = packetTimeMs(packet);
    const explicitQuaternion = window.THREE ? explicitQuaternionForPacket(packet) : null;
    const explicitAttitude = explicitAttitudeForPacket(packet);
    const tilt = accelerometerTilt(accelerometerVectorForPacket(packet));

    if (explicitQuaternion) {
      const quaternion = new window.THREE.Quaternion(
        explicitQuaternion.x,
        explicitQuaternion.y,
        explicitQuaternion.z,
        explicitQuaternion.w,
      ).normalize();
      attitude = attitudeFromQuaternion(quaternion);
      hasAttitudeData = true;
    } else if (explicitAttitude) {
      attitude = explicitAttitude;
      hasAttitudeData = true;
    } else {
      if (!attitude) {
        attitude = {
          roll: tilt?.roll || 0,
          pitch: tilt?.pitch || 0,
          yaw: 0,
        };
      }

      const gyro = gyroDegreesPerSecond(gyroVectorForPacket(packet), packet);

      if (gyro) {
        const rawStepSeconds = lastTimeMs !== null && packetMs !== null ? (packetMs - lastTimeMs) / 1000 : 1;
        const stepSeconds = clamp(rawStepSeconds, 0, MAX_GYRO_STEP_SECONDS);
        attitude.roll = wrapRadians(attitude.roll + degToRad(gyro.x * stepSeconds));
        attitude.pitch = wrapRadians(attitude.pitch + degToRad(gyro.y * stepSeconds));
        attitude.yaw = wrapRadians(attitude.yaw + degToRad(gyro.z * stepSeconds));
        hasAttitudeData = true;
      }
    }

    if (tilt && attitude) {
      attitude.roll = blendAngle(attitude.roll, tilt.roll, ATTITUDE_ACCEL_BLEND);
      attitude.pitch = blendAngle(attitude.pitch, tilt.pitch, ATTITUDE_ACCEL_BLEND);
      hasAttitudeData = true;
    }

    if (packetMs !== null) {
      lastTimeMs = packetMs;
    }
  });

  if (!hasAttitudeData || !attitude) {
    return null;
  }

  const quaternion = window.THREE ? attitudeQuaternionFromAngles(attitude) : null;

  return {
    ...attitude,
    gyro: latestGyro,
    quaternion,
    source: "gyro+accelerometer",
  };
}

function statusForPacket(packet) {
  const temperature = normalizeTemperature(packet?.sensors?.temperature);

  if (temperature !== null && temperature >= CRITICAL_TEMPERATURE) {
    return "Anomaly";
  }

  if (temperature !== null && temperature >= WARNING_TEMPERATURE) {
    return "Warning";
  }

  return "OK";
}

function latestTimestampFor(satellite) {
  const packet = latestPacketFor(satellite.satellite_id);
  return packet?.timestamp || satellite.latest_timestamp || null;
}

function statusForSatellite(satellite) {
  const packet = latestPacketFor(satellite.satellite_id);
  const age = secondsSince(latestTimestampFor(satellite));

  if (!Number.isFinite(age) || age > DISCONNECTED_PACKET_SECONDS) {
    return "Disconnected";
  }

  if (age > STALE_PACKET_SECONDS) {
    return "Late";
  }

  return statusForPacket(packet);
}

function statusClass(status) {
  return status.toLowerCase();
}

function selectedSatellite() {
  return state.satellites.find((satellite) => satellite.satellite_id === state.selectedSatelliteId) || null;
}

function selectedSatellitePackets() {
  return (state.telemetryBySatellite.get(state.selectedSatelliteId) || [])
    .slice()
    .sort((left, right) => String(right.timestamp).localeCompare(String(left.timestamp)));
}

function selectSatellite(satelliteId) {
  if (!satelliteId || satelliteId === state.selectedSatelliteId) {
    return;
  }

  state.selectedSatelliteId = satelliteId;
  saveSettings();
  render();
}

async function fetchJson(path, options = {}) {
  const response = await fetch(apiUrl(path), {
    method: options.method || "GET",
    headers: {
      Accept: "application/json",
      ...(options.body ? { "Content-Type": "application/json" } : {}),
      ...(options.headers || {}),
    },
    body: options.body ? JSON.stringify(options.body) : undefined,
  });

  const text = await response.text();
  let body = {};

  if (text) {
    body = JSON.parse(text);
  }

  if (!response.ok) {
    const message = body.message || body.error || response.statusText;
    throw new Error(message);
  }

  return body;
}

async function loadDashboard() {
  if (state.loading) {
    return;
  }

  state.loading = true;
  hideNotice();

  try {
    const [health, satelliteResult] = await Promise.all([
      fetchJson(""),
      fetchJson("/satellites"),
    ]);

    state.satellites = satelliteResult.satellites || [];

    if (!state.selectedSatelliteId && state.satellites[0]) {
      state.selectedSatelliteId = state.satellites[0].satellite_id;
    }

    await loadTelemetryForSatellites();

    els.apiState.textContent = `Connected to ${health.table || "SatMonDB"}`;
    els.healthStatus.textContent = "Online";
    els.healthStatus.className = "status-pill online";
    els.lastUpdate.textContent = formatClock();
    render();
  } catch (error) {
    els.apiState.textContent = "API unavailable";
    els.healthStatus.textContent = "Offline";
    els.healthStatus.className = "status-pill offline";
    showNotice(error.message || "Unable to load telemetry");
  } finally {
    state.loading = false;
  }
}

async function loadTelemetryForSatellites() {
  state.telemetryBySatellite.clear();

  const ids = state.satellites.map((satellite) => satellite.satellite_id).filter(Boolean);

  await Promise.all(ids.map(async (satelliteId) => {
    const limit = Math.max(state.packetLimit, 10);
    const result = await fetchJson(`/satellites/${encodeURIComponent(satelliteId)}/telemetry?limit=${limit}&order=desc`);
    state.telemetryBySatellite.set(satelliteId, result.items || []);
  }));
}

function normalizeColor(value) {
  const match = String(value || "").trim().match(/^#?[0-9a-fA-F]{6}$/);
  return match ? `#${match[0].replace("#", "").toLowerCase()}` : DEFAULT_LED_COLOR;
}

function normalizeLedTarget(value) {
  return value === LED_TARGET_ALL ? LED_TARGET_ALL : LED_TARGET_SELECTED;
}

function ledCommandTargetId() {
  return state.ledTarget === LED_TARGET_ALL ? LED_TARGET_ALL : state.selectedSatelliteId;
}

function ledCommandTargetLabel(targetId) {
  return targetId === LED_TARGET_ALL ? "all satellites" : targetId;
}

function hexToRgb(color) {
  const normalized = normalizeColor(color).slice(1);
  return {
    red: Number.parseInt(normalized.slice(0, 2), 16),
    green: Number.parseInt(normalized.slice(2, 4), 16),
    blue: Number.parseInt(normalized.slice(4, 6), 16),
  };
}

function setLedStatus(message, tone = "idle") {
  els.ledStatus.textContent = message;
  els.ledStatus.className = `led-status ${tone}`;
}

function setLedLoading(isLoading) {
  state.ledLoading = isLoading;
  els.sendLedButton.disabled = isLoading || !ledCommandTargetId();
  setIconButton(els.sendLedButton, isLoading ? "loader-circle" : "lightbulb", isLoading ? "Sending" : "Apply LED");
}

function renderLedControls() {
  state.ledTarget = normalizeLedTarget(state.ledTarget);
  state.ledColor = normalizeColor(state.ledColor);
  els.ledEnabled.checked = state.ledEnabled;
  els.ledTarget.value = state.ledTarget;
  els.ledColor.value = state.ledColor;
  els.ledPreview.style.setProperty("--led-color", state.ledColor);
  els.ledPreview.classList.toggle("off", !state.ledEnabled);
  els.sendLedButton.disabled = state.ledLoading || !ledCommandTargetId();
}

async function publishLedCommand() {
  state.ledTarget = normalizeLedTarget(els.ledTarget.value);

  const targetId = ledCommandTargetId();
  if (!targetId) {
    showNotice("Select a satellite or choose all satellites before sending an LED command.");
    return;
  }

  state.ledEnabled = els.ledEnabled.checked;
  state.ledColor = normalizeColor(els.ledColor.value);
  const targetLabel = ledCommandTargetLabel(targetId);
  saveSettings();
  renderLedControls();
  hideNotice();
  setLedLoading(true);
  setLedStatus("Cloud sending", "pending");

  try {
    const { red, green, blue } = hexToRgb(state.ledColor);
    const result = await fetchJson(`/satellites/${encodeURIComponent(targetId)}/commands/led`, {
      method: "POST",
      body: {
        satellite_id: targetId,
        command: "led",
        enabled: state.ledEnabled,
        color: state.ledColor,
        red,
        green,
        blue,
      },
    });

    setLedStatus(`Queued ${targetLabel}`, "ok");
    showNotice(`LED command queued through cloud for ${targetLabel}. Topic: ${result.topic || "satmon/commands"}.`);
  } catch (error) {
    setLedStatus("Cloud failed", "error");
    showNotice(error.message || "Unable to queue LED command in cloud");
  } finally {
    setLedLoading(false);
  }
}

function showNotice(message) {
  els.notice.textContent = message;
  els.notice.hidden = false;
}

function hideNotice() {
  els.notice.hidden = true;
  els.notice.textContent = "";
}

function ensureSelectedSatellite() {
  const ids = state.satellites.map((satellite) => satellite.satellite_id);

  if (!ids.includes(state.selectedSatelliteId)) {
    state.selectedSatelliteId = ids[0] || "";
  }
}

function renderControls() {
  ensureSelectedSatellite();

  els.apiEndpoint.value = state.endpoint;
  els.selectedSatelliteLabel.textContent = state.selectedSatelliteId || "--";
  els.packetLimit.value = String(state.packetLimit);
  els.autoRefresh.checked = state.autoRefresh;
}

function renderMetrics(alerts) {
  const satellite = selectedSatellite();
  const packets = selectedSatellitePackets();
  const onlineCount = state.satellites.filter((candidate) => statusForSatellite(candidate) !== "Disconnected").length;
  const temperatures = packets
    .map((packet) => normalizeTemperature(packet?.sensors?.temperature))
    .filter((temperature) => temperature !== null);

  const average = temperatures.length
    ? temperatures.reduce((total, value) => total + value, 0) / temperatures.length
    : null;

  els.activeSatellites.textContent = state.satellites.length ? `${onlineCount}/${state.satellites.length}` : "--";
  els.activeAlerts.textContent = String(alerts.length);
  els.averageTemperature.textContent = average === null ? "--" : `${average.toFixed(1)} C`;
  els.packetCount.textContent = String(satellite?.messages || packets.length);
}

function latestPacketFor(satelliteId) {
  const packets = state.telemetryBySatellite.get(satelliteId) || [];
  return packets[0] || null;
}

function renderSatellites() {
  els.satelliteGrid.replaceChildren();

  if (state.satellites.length === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state compact";
    empty.textContent = "No satellite data received";
    els.satelliteGrid.append(empty);
    return;
  }

  state.satellites.forEach((satellite) => {
    const packet = latestPacketFor(satellite.satellite_id);
    const status = statusForSatellite(satellite);
    const isSelected = satellite.satellite_id === state.selectedSatelliteId;
    const latestTimestamp = latestTimestampFor(satellite);
    const mpu = packet?.sensors?.mpu || {};

    const card = document.createElement("article");
    card.className = `satellite-card ${statusClass(status)}${isSelected ? " selected" : ""}`;
    card.tabIndex = 0;
    card.role = "button";
    card.setAttribute("aria-pressed", String(isSelected));
    card.setAttribute("aria-label", `Select ${satellite.satellite_id}`);
    card.innerHTML = `
      <div class="satellite-card-head">
        <strong></strong>
        <span class="badge"></span>
      </div>
      <dl class="satellite-readings">
        <div><dt>Temperature</dt><dd></dd></div>
        <div><dt>Link</dt><dd></dd></div>
        <div><dt>Packets</dt><dd></dd></div>
        <div><dt>Updated</dt><dd></dd></div>
      </dl>
    `;

    card.querySelector("strong").textContent = satellite.satellite_id;
    const badge = card.querySelector(".badge");
    badge.textContent = status;
    badge.classList.add(statusClass(status));

    const values = card.querySelectorAll("dd");
    values[0].textContent = formatTemperature(packet?.sensors?.temperature);
    values[1].textContent = status === "Disconnected" ? "No signal" : status === "Late" ? "Late" : "Connected";
    values[2].textContent = String(satellite.messages || (state.telemetryBySatellite.get(satellite.satellite_id) || []).length);
    values[3].textContent = relativeAgeText(latestTimestamp);

    card.addEventListener("click", () => selectSatellite(satellite.satellite_id));
    card.addEventListener("keydown", (event) => {
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        selectSatellite(satellite.satellite_id);
      }
    });

    els.satelliteGrid.append(card);
  });
}

function buildAlerts() {
  const alerts = [];

  state.satellites.forEach((satellite) => {
    const packet = latestPacketFor(satellite.satellite_id);
    const temperature = normalizeTemperature(packet?.sensors?.temperature);
    const age = secondsSince(latestTimestampFor(satellite));

    if (!Number.isFinite(age) || age > DISCONNECTED_PACKET_SECONDS) {
      alerts.push({
        satelliteId: satellite.satellite_id,
        level: "Disconnected",
        message: `No telemetry for ${relativeAgeText(latestTimestampFor(satellite)).toLowerCase()}`,
      });
      return;
    }

    if (age > STALE_PACKET_SECONDS) {
      alerts.push({
        satelliteId: satellite.satellite_id,
        level: "Late",
        message: `Last telemetry ${relativeAgeText(latestTimestampFor(satellite)).toLowerCase()}`,
      });
    }

    if (temperature !== null && temperature >= CRITICAL_TEMPERATURE) {
      alerts.push({
        satelliteId: satellite.satellite_id,
        level: "Anomaly",
        message: `Temperature reached ${temperature.toFixed(1)} C`,
      });
    } else if (temperature !== null && temperature >= WARNING_TEMPERATURE) {
      alerts.push({
        satelliteId: satellite.satellite_id,
        level: "Warning",
        message: `Temperature is ${temperature.toFixed(1)} C`,
      });
    }
  });

  return alerts;
}

function renderAlerts(alerts) {
  els.alertList.replaceChildren();

  if (alerts.length === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state compact";
    empty.textContent = "No active alerts";
    els.alertList.append(empty);
    return;
  }

  alerts.forEach((alert) => {
    const row = document.createElement("article");
    row.className = `alert-row ${statusClass(alert.level)}`;
    row.innerHTML = "<span></span><div><strong></strong><p></p></div>";
    row.querySelector("strong").textContent = `${alert.satelliteId} ${alert.level}`;
    row.querySelector("p").textContent = alert.message;
    els.alertList.append(row);
  });
}

function renderPackets() {
  const packets = selectedSatellitePackets().slice(0, state.packetLimit);
  els.packetTable.replaceChildren();

  if (packets.length === 0) {
    const row = document.createElement("tr");
    row.innerHTML = '<td colspan="6" class="table-empty">No packets available</td>';
    els.packetTable.append(row);
    return;
  }

  packets.forEach((packet) => {
    const mpu = packet?.sensors?.mpu || {};
    const status = statusForPacket(packet);
    const row = document.createElement("tr");
    row.innerHTML = `
      <td></td>
      <td></td>
      <td></td>
      <td></td>
      <td></td>
      <td><span class="badge"></span></td>
    `;

    const cells = row.querySelectorAll("td");
    cells[0].textContent = packet.satellite_id || "--";
    cells[1].textContent = formatTimestamp(packet.timestamp);
    cells[2].textContent = formatTemperature(packet?.sensors?.temperature);
    cells[3].textContent = compactVector(mpu.accelerometer);
    cells[4].textContent = compactVector(mpu.gyroscope);

    const badge = row.querySelector(".badge");
    badge.textContent = status;
    badge.classList.add(statusClass(status));

    els.packetTable.append(row);
  });
}

function drawHistoryChart() {
  const canvas = els.historyChart;
  const context = canvas.getContext("2d");
  const rect = canvas.getBoundingClientRect();
  const pixelRatio = window.devicePixelRatio || 1;

  canvas.width = Math.max(1, Math.floor(rect.width * pixelRatio));
  canvas.height = Math.max(1, Math.floor(rect.height * pixelRatio));
  context.setTransform(pixelRatio, 0, 0, pixelRatio, 0, 0);

  const width = rect.width;
  const height = rect.height;
  const padding = { top: 18, right: 18, bottom: 28, left: 42 };
  const packets = (state.telemetryBySatellite.get(state.selectedSatelliteId) || [])
    .slice(0, state.packetLimit)
    .map((packet) => ({
      timestamp: packet.timestamp,
      temperature: normalizeTemperature(packet?.sensors?.temperature),
    }))
    .filter((point) => point.temperature !== null)
    .reverse();

  context.clearRect(0, 0, width, height);
  context.fillStyle = cssColor("--chart-bg", "#fbfdfb");
  context.fillRect(0, 0, width, height);

  context.strokeStyle = cssColor("--chart-grid", "#d8e3de");
  context.lineWidth = 1;

  for (let line = 0; line <= 4; line += 1) {
    const y = padding.top + ((height - padding.top - padding.bottom) * line) / 4;
    context.beginPath();
    context.moveTo(padding.left, y);
    context.lineTo(width - padding.right, y);
    context.stroke();
  }

  if (packets.length < 2) {
    context.fillStyle = cssColor("--muted", "#61706b");
    context.font = "600 13px Inter, system-ui, sans-serif";
    context.textAlign = "center";
    context.fillText("Waiting for telemetry history", width / 2, height / 2);
    return;
  }

  const values = packets.map((point) => point.temperature);
  const min = Math.min(...values, WARNING_TEMPERATURE - 5);
  const max = Math.max(...values, CRITICAL_TEMPERATURE + 5);
  const plotWidth = width - padding.left - padding.right;
  const plotHeight = height - padding.top - padding.bottom;

  function xFor(index) {
    return padding.left + (plotWidth * index) / (packets.length - 1);
  }

  function yFor(value) {
    return padding.top + plotHeight - ((value - min) / (max - min || 1)) * plotHeight;
  }

  context.strokeStyle = cssColor("--teal", "#177e89");
  context.lineWidth = 2.5;
  context.beginPath();

  packets.forEach((point, index) => {
    const x = xFor(index);
    const y = yFor(point.temperature);

    if (index === 0) {
      context.moveTo(x, y);
    } else {
      context.lineTo(x, y);
    }
  });

  context.stroke();

  context.fillStyle = cssColor("--teal", "#177e89");
  packets.forEach((point, index) => {
    context.beginPath();
    context.arc(xFor(index), yFor(point.temperature), 3, 0, Math.PI * 2);
    context.fill();
  });

  context.fillStyle = cssColor("--muted", "#61706b");
  context.font = "600 11px Inter, system-ui, sans-serif";
  context.textAlign = "left";
  context.fillText(`${max.toFixed(0)} C`, 8, padding.top + 4);
  context.fillText(`${min.toFixed(0)} C`, 8, height - padding.bottom + 4);

  context.textAlign = "center";
  context.fillText(state.selectedSatelliteId || "Satellite", width / 2, height - 8);
}

function createReferenceFrame(THREE) {
  const geometry = new THREE.BufferGeometry().setFromPoints([
    new THREE.Vector3(-2.4, 0, 0),
    new THREE.Vector3(2.4, 0, 0),
    new THREE.Vector3(0, -1.8, 0),
    new THREE.Vector3(0, 1.8, 0),
    new THREE.Vector3(0, 0, -2.2),
    new THREE.Vector3(0, 0, 2.2),
  ]);
  const material = new THREE.LineBasicMaterial({ color: 0x86a69c, transparent: true, opacity: 0.42 });
  return new THREE.LineSegments(geometry, material);
}

function createSatelliteModel(THREE) {
  const satellite = new THREE.Group();
  const metal = new THREE.MeshStandardMaterial({ color: 0xe4ebe6, metalness: 0.46, roughness: 0.34 });
  const darkMetal = new THREE.MeshStandardMaterial({ color: 0x62746e, metalness: 0.55, roughness: 0.36 });
  const panel = new THREE.MeshStandardMaterial({ color: 0x177e89, metalness: 0.22, roughness: 0.28 });
  const accent = new THREE.MeshStandardMaterial({ color: 0xf2b544, metalness: 0.36, roughness: 0.3 });

  const bus = new THREE.Mesh(new THREE.BoxGeometry(1.05, 0.78, 0.82), metal);
  satellite.add(bus);

  const top = new THREE.Mesh(new THREE.BoxGeometry(0.72, 0.08, 0.72), accent);
  top.position.y = 0.43;
  satellite.add(top);

  const boom = new THREE.Mesh(new THREE.CylinderGeometry(0.025, 0.025, 3.55, 18), darkMetal);
  boom.rotation.z = Math.PI / 2;
  satellite.add(boom);

  [-1, 1].forEach((side) => {
    const solarPanel = new THREE.Mesh(new THREE.BoxGeometry(1.22, 0.045, 0.72), panel);
    solarPanel.position.x = side * 1.28;
    satellite.add(solarPanel);

    const rib = new THREE.Mesh(new THREE.BoxGeometry(0.035, 0.06, 0.78), darkMetal);
    rib.position.x = side * 1.28;
    satellite.add(rib);
  });

  const dish = new THREE.Mesh(new THREE.ConeGeometry(0.24, 0.36, 32), darkMetal);
  dish.rotation.x = Math.PI / 2;
  dish.position.z = -0.63;
  satellite.add(dish);

  const antenna = new THREE.Mesh(new THREE.CylinderGeometry(0.018, 0.018, 0.7, 14), darkMetal);
  antenna.position.y = 0.82;
  satellite.add(antenna);

  return satellite;
}

function resizeAttitudeView() {
  if (!attitudeView) {
    return;
  }

  const { canvas, camera, renderer } = attitudeView;
  const rect = canvas.getBoundingClientRect();
  const width = Math.max(1, rect.width);
  const height = Math.max(1, rect.height);

  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.setSize(width, height, false);
  camera.aspect = width / height;
  camera.updateProjectionMatrix();
}

function initAttitudeView() {
  if (attitudeView || !els.attitudeCanvas || !window.THREE) {
    return attitudeView;
  }

  const THREE = window.THREE;
  const canvas = els.attitudeCanvas;
  const renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(38, 1, 0.1, 100);
  const satellite = createSatelliteModel(THREE);
  const targetQuaternion = new THREE.Quaternion();

  scene.background = new THREE.Color(0x071f1c);
  camera.position.set(4.6, 2.7, 5.2);
  camera.lookAt(0, 0, 0);

  scene.add(new THREE.HemisphereLight(0xf9fff9, 0x16312d, 2.2));
  const keyLight = new THREE.DirectionalLight(0xffffff, 2.4);
  keyLight.position.set(3.5, 4.5, 4.2);
  scene.add(keyLight);
  scene.add(createReferenceFrame(THREE));
  scene.add(satellite);

  attitudeView = { canvas, camera, renderer, satellite, targetQuaternion };
  resizeAttitudeView();

  renderer.setAnimationLoop(() => {
    satellite.quaternion.slerp(targetQuaternion, 0.18);
    renderer.render(scene, camera);
  });

  return attitudeView;
}

function renderAttitudeReadout(attitude) {
  els.attitudeSatellite.textContent = state.selectedSatelliteId || "--";
  els.attitudeRoll.textContent = attitude ? formatDegrees(radToDeg(attitude.roll)) : "--";
  els.attitudePitch.textContent = attitude ? formatDegrees(radToDeg(attitude.pitch)) : "--";
  els.attitudeYaw.textContent = attitude ? formatDegrees(radToDeg(attitude.yaw)) : "--";
  els.attitudeGyro.textContent = formatGyroVector(attitude?.gyro);
}

function renderAttitudeView() {
  const view = initAttitudeView();
  const packets = state.telemetryBySatellite.get(state.selectedSatelliteId) || [];
  const attitude = deriveAttitude(packets);

  renderAttitudeReadout(attitude);

  if (els.attitudeEmpty) {
    els.attitudeEmpty.hidden = Boolean(attitude && view);
  }

  if (!view) {
    if (els.attitudeEmpty) {
      els.attitudeEmpty.textContent = "3D view unavailable";
    }
    return;
  }

  if (attitude?.quaternion) {
    view.targetQuaternion.copy(attitude.quaternion);
  } else {
    view.targetQuaternion.identity();
  }
}

function render() {
  const alerts = buildAlerts();
  renderControls();
  renderLedControls();
  renderMetrics(alerts);
  renderSatellites();
  renderAlerts(alerts);
  renderPackets();
  drawHistoryChart();
  renderAttitudeView();
  saveSettings();
}

function scheduleRefresh() {
  window.clearInterval(state.refreshTimer);

  if (state.autoRefresh) {
    state.refreshTimer = window.setInterval(loadDashboard, REFRESH_INTERVAL_MS);
  }
}

function applyEndpointSetting() {
  const endpoint = normalizeEndpoint(els.apiEndpoint.value || DEFAULT_API_ENDPOINT);

  if (endpoint === state.endpoint) {
    return;
  }

  state.endpoint = endpoint;
  saveSettings();
  loadDashboard();
}

els.controlForm.addEventListener("submit", (event) => {
  event.preventDefault();
  applyEndpointSetting();
});

els.apiEndpoint.addEventListener("change", applyEndpointSetting);
els.apiEndpoint.addEventListener("blur", applyEndpointSetting);

els.packetLimit.addEventListener("change", () => {
  state.packetLimit = Number(els.packetLimit.value || 25);
  saveSettings();
  loadDashboard();
});

els.ledForm.addEventListener("submit", (event) => {
  event.preventDefault();
  publishLedCommand();
});

els.ledEnabled.addEventListener("change", () => {
  state.ledEnabled = els.ledEnabled.checked;
  saveSettings();
  renderLedControls();
});

els.ledTarget.addEventListener("change", () => {
  state.ledTarget = normalizeLedTarget(els.ledTarget.value);
  saveSettings();
  renderLedControls();
});

els.ledColor.addEventListener("input", () => {
  state.ledColor = normalizeColor(els.ledColor.value);
  saveSettings();
  renderLedControls();
});

els.autoRefresh.addEventListener("change", () => {
  state.autoRefresh = els.autoRefresh.checked;
  saveSettings();
  scheduleRefresh();
});

window.addEventListener("resize", () => {
  drawHistoryChart();
  resizeAttitudeView();
});

els.apiEndpoint.value = state.endpoint;
els.packetLimit.value = String(state.packetLimit);
els.autoRefresh.checked = state.autoRefresh;
renderLedControls();
refreshIcons();
scheduleRefresh();
loadDashboard();
