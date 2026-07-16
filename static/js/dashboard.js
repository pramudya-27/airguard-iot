/**
 * AIRGUARD - Dashboard Interactivity (Single Page Application)
 * Handles client-side routing, page transition animations,
 * Chart.js rendering, live 2s telemetry updates, and database actions.
 */

const API_BASE = '/api';

/* ==========================================================================
   BROWSER MQTT CLIENT (For Vercel Serverless Compatibility)
   Connects directly from browser to HiveMQ Cloud via WebSocket (wss://)
   and forwards incoming telemetry to the Flask backend via /api/log
   ========================================================================== */
const MQTT_CONFIG = {
    host: '712b2488276943edbd55e938ca1ba12b.s1.eu.hivemq.cloud',
    port: 8884,           // HiveMQ Cloud WSS port
    username: 'mikail',
    password: 'mikail123',
    deviceId: 'AIRGUARD-01',
    clientId: 'airguard-browser-' + Math.random().toString(16).substring(2, 8)
};

let mqttClient = null;
let mqttConnected = false;

const initBrowserMQTT = () => {
    if (typeof Paho === 'undefined') {
        console.warn('Paho MQTT library not loaded, skipping browser MQTT.');
        return;
    }

    mqttClient = new Paho.MQTT.Client(MQTT_CONFIG.host, MQTT_CONFIG.port, MQTT_CONFIG.clientId);

    mqttClient.onConnectionLost = (responseObject) => {
        console.warn('Browser MQTT connection lost:', responseObject.errorMessage);
        mqttConnected = false;
        // Update status badge immediately
        const mqttBadge = document.getElementById('mqtt-status');
        if (mqttBadge) {
            mqttBadge.textContent = 'DISCONNECTED';
            mqttBadge.className = 'badge badge-danger';
        }
        // Reconnect after 5 seconds
        setTimeout(initBrowserMQTT, 5000);
    };

    mqttClient.onMessageArrived = async (message) => {
        try {
            const topic = message.destinationName;
            const payload = JSON.parse(message.payloadString);
            const deviceId = MQTT_CONFIG.deviceId;

            if (topic === `airguard/${deviceId}/telemetry`) {
                // Update UI immediately without waiting for API
                updateDashboardUI(
                    { ...payload, mqtt_connected: true },
                    { status: 'ONLINE', last_seen: new Date().toISOString() }
                );
                updateChart(
                    new Date().toISOString(),
                    payload.temperature,
                    payload.humidity,
                    payload.gas_percentage
                );
                // Forward to backend to save in database
                fetch(`${API_BASE}/log`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(payload)
                }).catch(err => console.error('Error forwarding telemetry to backend:', err));
            }

            if (topic === `airguard/${deviceId}/status`) {
                const statusVal = payload.status || 'OFFLINE';
                const deviceBadge = document.getElementById('device-status');
                if (deviceBadge) {
                    deviceBadge.textContent = statusVal.replace('_', ' ');
                    if (statusVal === 'ONLINE') deviceBadge.className = 'badge badge-success';
                    else if (statusVal === 'WARMING_UP') deviceBadge.className = 'badge badge-warning';
                    else deviceBadge.className = 'badge badge-danger';
                }
            }

            if (topic === `airguard/${deviceId}/alert`) {
                const alertBanner = document.getElementById('alert-banner');
                if (alertBanner) alertBanner.classList.remove('hidden');
            }

        } catch (err) {
            console.error('Error processing MQTT message:', err);
        }
    };

    const connectOptions = {
        useSSL: true,
        userName: MQTT_CONFIG.username,
        password: MQTT_CONFIG.password,
        keepAliveInterval: 30,
        cleanSession: true,
        onSuccess: () => {
            console.log('Browser MQTT connected to HiveMQ Cloud via WSS!');
            mqttConnected = true;

            // Update MQTT status badge
            const mqttBadge = document.getElementById('mqtt-status');
            if (mqttBadge) {
                mqttBadge.textContent = 'CONNECTED';
                mqttBadge.className = 'badge badge-success';
            }

            // Subscribe to all device topics
            const deviceId = MQTT_CONFIG.deviceId;
            mqttClient.subscribe(`airguard/${deviceId}/telemetry`);
            mqttClient.subscribe(`airguard/${deviceId}/status`);
            mqttClient.subscribe(`airguard/${deviceId}/alert`);
        },
        onFailure: (err) => {
            console.error('Browser MQTT connection failed:', err.errorMessage);
            mqttConnected = false;
            // Retry after 5 seconds
            setTimeout(initBrowserMQTT, 5000);
        }
    };

    try {
        mqttClient.connect(connectOptions);
    } catch (err) {
        console.error('Error connecting to MQTT:', err);
    }
};

// Expose publishCommand so device controls can send commands via browser MQTT
const publishMQTTCommand = (command) => {
    if (!mqttClient || !mqttConnected) {
        console.warn('Browser MQTT not connected, cannot publish command.');
        return false;
    }
    const deviceId = MQTT_CONFIG.deviceId;
    const msg = new Paho.MQTT.Message(JSON.stringify({ command }));
    msg.destinationName = `airguard/${deviceId}/command`;
    msg.qos = 1;
    mqttClient.send(msg);
    return true;
};

const publishMQTTSettings = (settings) => {
    if (!mqttClient || !mqttConnected) return false;
    const deviceId = MQTT_CONFIG.deviceId;
    const msg = new Paho.MQTT.Message(JSON.stringify(settings));
    msg.destinationName = `airguard/${deviceId}/settings`;
    msg.qos = 1;
    mqttClient.send(msg);
    return true;
};

// --- Utility Functions ---
const showNotification = (message, isError = false) => {
    alert((isError ? "Error: " : "Success: ") + message);
};

/* ==========================================================================
   GLOBAL VARIABLES & STATES
   ========================================================================== */
let mainChart = null;
const MAX_DATA_POINTS = 30;
let dashboardInterval = null;

// History state
let historyData = [];
let filteredData = [];
let currentPage = 1;
const pageSize = 10;

/* ==========================================================================
   DASHBOARD MODULE
   ========================================================================== */
const initChart = () => {
    const canvas = document.getElementById('mainChart');
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    mainChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: [],
            datasets: [
                {
                    label: 'Suhu (°C)',
                    borderColor: '#2563eb', // Blue
                    backgroundColor: 'rgba(37, 99, 235, 0.1)',
                    data: [],
                    borderWidth: 2,
                    tension: 0.3,
                    pointRadius: 0,
                },
                {
                    label: 'Kelembapan (%)',
                    borderColor: '#10b981', // Green
                    backgroundColor: 'rgba(16, 185, 129, 0.1)',
                    data: [],
                    borderWidth: 2,
                    tension: 0.3,
                    pointRadius: 0,
                },
                {
                    label: 'Gas/Asap (%)',
                    borderColor: '#f59e0b', // Yellow
                    backgroundColor: 'rgba(245, 158, 11, 0.1)',
                    data: [],
                    borderWidth: 2,
                    tension: 0.3,
                    pointRadius: 0,
                }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: false,
            scales: {
                x: { grid: { display: false } },
                y: {
                    beginAtZero: true,
                    max: 100,
                    grid: { borderDash: [5, 5] }
                }
            },
            interaction: {
                mode: 'index',
                intersect: false,
            },
        }
    });
};

const updateChart = (timestamp, temp, hum, gasPercent) => {
    if (!mainChart) return;
    const timeLabel = new Date(timestamp).toLocaleTimeString('id-ID', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    
    mainChart.data.labels.push(timeLabel);
    mainChart.data.datasets[0].data.push(temp);
    mainChart.data.datasets[1].data.push(hum);
    mainChart.data.datasets[2].data.push(gasPercent);

    if (mainChart.data.labels.length > MAX_DATA_POINTS) {
        mainChart.data.labels.shift();
        mainChart.data.datasets.forEach(dataset => dataset.data.shift());
    }

    mainChart.update();
};

const updateDashboardUI = (data, statusData) => {
    // Status Badges
    const deviceBadge = document.getElementById('device-status');
    if (statusData && statusData.status) {
        const statusUpper = statusData.status.toUpperCase();
        deviceBadge.textContent = statusUpper.replace('_', ' '); // format WARMING_UP to WARMING UP
        if (statusUpper === 'ONLINE') {
            deviceBadge.className = 'badge badge-success';
        } else if (statusUpper === 'WARMING_UP') {
            deviceBadge.className = 'badge badge-warning';
        } else {
            deviceBadge.className = 'badge badge-danger';
        }
    }
    
    document.getElementById('mqtt-status').textContent = data.mqtt_connected ? "CONNECTED" : "DISCONNECTED";
    document.getElementById('mqtt-status').className = data.mqtt_connected ? 'badge badge-success' : 'badge badge-danger';
    
    if (data.timestamp) {
        document.getElementById('last-update').textContent = new Date(data.timestamp).toLocaleString('id-ID');
    }

    // Sensor Values
    document.getElementById('val-temp').textContent = data.temperature !== null ? data.temperature.toFixed(1) : '--';
    document.getElementById('val-hum').textContent = data.humidity !== null ? data.humidity.toFixed(1) : '--';
    
    // Air Status
    const airStatusEl = document.getElementById('val-air-status');
    airStatusEl.textContent = data.air_status || 'UNKNOWN';
    airStatusEl.className = 'status-text';
    if (data.air_status === 'BAIK') airStatusEl.classList.add('green');
    else if (data.air_status === 'SEDANG') airStatusEl.classList.add('yellow');
    else if (data.air_status === 'BURUK') airStatusEl.classList.add('orange');
    else if (data.air_status === 'BAHAYA') airStatusEl.classList.add('red');

    // Buzzer Status
    document.getElementById('val-buzzer').textContent = data.buzzer_status ? 'ON' : 'OFF';
    document.getElementById('val-rssi').textContent = data.rssi || '--';

    // SVG Gauge updates
    const gasPercent = data.gas_percentage || 0;
    document.getElementById('val-gas-percent').textContent = gasPercent;
    
    const maxOffset = 125.66;
    const offset = maxOffset - (gasPercent / 100) * maxOffset;
    const fillEl = document.getElementById('gauge-fill');
    if (fillEl) fillEl.setAttribute('stroke-dashoffset', offset);

    document.getElementById('val-raw').textContent = data.gas_raw || 0;
    document.getElementById('val-baseline').textContent = data.gas_baseline || 0;
    document.getElementById('val-delta').textContent = data.gas_delta || 0;

    // Alert Banner
    const alertBanner = document.getElementById('alert-banner');
    if (data.danger_active) {
        alertBanner.classList.remove('hidden');
    } else {
        alertBanner.classList.add('hidden');
    }
};

const fetchLatestData = async () => {
    try {
        const [latestRes, statusRes] = await Promise.all([
            fetch(`${API_BASE}/latest`),
            fetch(`${API_BASE}/status`)
        ]);

        if (latestRes.ok && statusRes.ok) {
            const latestData = await latestRes.json();
            const statusData = await statusRes.json();
            
            updateDashboardUI(latestData, statusData);
            
            const timestamp = latestData.timestamp || new Date().toISOString();
            updateChart(timestamp, latestData.temperature, latestData.humidity, latestData.gas_percentage);
        }
    } catch (error) {
        console.error("Error fetching latest data:", error);
        const deviceBadge = document.getElementById('device-status');
        if (deviceBadge) {
            deviceBadge.textContent = 'OFFLINE';
            deviceBadge.className = 'badge badge-danger';
        }
    }
};

const startDashboardPolling = () => {
    if (!dashboardInterval) {
        fetchLatestData(); // Fetch immediately
        dashboardInterval = setInterval(fetchLatestData, 2000);
    }
};

const stopDashboardPolling = () => {
    if (dashboardInterval) {
        clearInterval(dashboardInterval);
        dashboardInterval = null;
    }
};

const setupControls = () => {
    // Map endpoint to MQTT command string
    const commandMap = {
        '/buzzer/test': 'TEST_BUZZER',
        '/buzzer/silence': 'SILENCE_BUZZER',
        '/alarm/reset': 'RESET_ALARM',
        '/calibrate': 'CALIBRATE_BASELINE'
    };

    const sendCommand = async (endpoint) => {
        // Try browser MQTT first (works on Vercel)
        const mqttCmd = commandMap[endpoint];
        if (mqttCmd && publishMQTTCommand(mqttCmd)) {
            // Also hit HTTP API for side-effects (e.g. reset_danger_state in DB)
            fetch(`${API_BASE}${endpoint}`, { method: 'POST' }).catch(() => {});
            return;
        }
        // Fallback to HTTP API (works on local)
        try {
            const res = await fetch(`${API_BASE}${endpoint}`, { method: 'POST' });
            const data = await res.json();
            if (!res.ok) {
                showNotification(data.message || "Gagal mengirim perintah", true);
            }
        } catch (error) {
            console.error(`Error sending command to ${endpoint}:`, error);
            showNotification("Terjadi kesalahan jaringan", true);
        }
    };

    document.getElementById('btn-test-buzzer').addEventListener('click', () => sendCommand('/buzzer/test'));
    document.getElementById('btn-silence-buzzer').addEventListener('click', () => sendCommand('/buzzer/silence'));
    document.getElementById('btn-reset-alarm').addEventListener('click', () => sendCommand('/alarm/reset'));
    document.getElementById('btn-calibrate').addEventListener('click', () => {
        if (confirm("Apakah Anda yakin ingin melakukan kalibrasi baseline? Pastikan sensor berada di udara bersih.")) {
            sendCommand('/calibrate');
        }
    });
};

/* ==========================================================================
   HISTORY MODULE
   ========================================================================== */
const getStatusColorClass = (status) => {
    if (status === 'BAIK') return 'green';
    if (status === 'SEDANG') return 'yellow';
    if (status === 'BURUK') return 'orange';
    if (status === 'BAHAYA') return 'red';
    return '';
};

const renderTable = () => {
    const tbody = document.getElementById('history-tbody');
    if (!tbody) return;
    tbody.innerHTML = '';

    const start = (currentPage - 1) * pageSize;
    const end = start + pageSize;
    const pageData = filteredData.slice(start, end);

    if (filteredData.length === 0) {
        tbody.innerHTML = '<tr><td colspan="9" style="text-align: center;">Tidak ada data ditemukan</td></tr>';
        updatePaginationUI();
        return;
    }

    pageData.forEach((row, index) => {
        const dateObj = new Date(row.timestamp);
        const dateStr = dateObj.toLocaleDateString('id-ID');
        const timeStr = dateObj.toLocaleTimeString('id-ID', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
        
        const isDanger = row.air_status === 'BAHAYA';
        const rowStyle = isDanger ? 'style="background-color: rgba(239, 68, 68, 0.1);"' : '';

        const tr = document.createElement('tr');
        tr.innerHTML = `
            <td ${rowStyle}>${start + index + 1}</td>
            <td ${rowStyle}>${dateStr}</td>
            <td ${rowStyle}>${timeStr}</td>
            <td ${rowStyle}>${row.temperature !== null ? row.temperature.toFixed(1) : '--'}</td>
            <td ${rowStyle}>${row.humidity !== null ? row.humidity.toFixed(1) : '--'}</td>
            <td ${rowStyle}>${row.gas_raw} (${row.gas_percentage}%)</td>
            <td ${rowStyle}><span class="status-text ${getStatusColorClass(row.air_status)}">${row.air_status}</span></td>
            <td ${rowStyle}>${row.buzzer_status ? '<i class="fa-solid fa-volume-high icon-blue"></i> ON' : 'OFF'}</td>
            <td ${rowStyle}>
                <button class="btn-delete-row" data-id="${row.id}" title="Hapus data ini">
                    <i class="fa-solid fa-trash-can"></i>
                </button>
            </td>
        `;
        tbody.appendChild(tr);
    });

    document.querySelectorAll('.btn-delete-row').forEach(btn => {
        btn.addEventListener('click', async () => {
            const logId = btn.getAttribute('data-id');
            if (confirm('Apakah Anda yakin ingin menghapus baris data ini?')) {
                await deleteLog(logId);
            }
        });
    });

    updatePaginationUI();
};

const updatePaginationUI = () => {
    const start = (currentPage - 1) * pageSize;
    const end = start + pageSize;
    const total = filteredData.length;

    const infoText = `Menampilkan ${total ? start + 1 : 0} - ${Math.min(end, total)} dari ${total} data`;
    document.getElementById('pagination-info').textContent = infoText;

    document.getElementById('btn-prev').disabled = currentPage <= 1;
    document.getElementById('btn-next').disabled = end >= total;
};

const fetchHistory = async () => {
    try {
        const summarize = document.getElementById('view-mode-select').value;
        const res = await fetch(`${API_BASE}/history?summarize=${summarize}`);
        if (res.ok) {
            historyData = await res.json();
            applyFilters();
        }
    } catch (error) {
        console.error("Error fetching history:", error);
    }
};

const applyFilters = () => {
    const statusFilter = document.getElementById('status-filter');
    const tempFilter = document.getElementById('temp-filter');
    const humFilter = document.getElementById('hum-filter');
    const dateInput = document.getElementById('date-filter');
    
    if (!statusFilter || !tempFilter || !humFilter || !dateInput) return;

    const statusVal = statusFilter.value;
    const tempVal = tempFilter.value;
    const humVal = humFilter.value;
    const dateVal = dateInput.value;

    filteredData = historyData.filter(row => {
        const dateObj = new Date(row.timestamp);
        const rowDateStr = dateObj.toISOString().split('T')[0];
        
        // 1. Filter Status Udara
        const matchStatus = statusVal === 'ALL' || row.air_status === statusVal;

        // 2. Filter Suhu
        let matchTemp = true;
        if (row.temperature !== null) {
            if (tempVal === 'COLD') matchTemp = row.temperature < 25;
            else if (tempVal === 'NORMAL') matchTemp = row.temperature >= 25 && row.temperature <= 30;
            else if (tempVal === 'HOT') matchTemp = row.temperature > 30;
        }

        // 3. Filter Kelembapan
        let matchHum = true;
        if (row.humidity !== null) {
            if (humVal === 'DRY') matchHum = row.humidity < 50;
            else if (humVal === 'NORMAL') matchHum = row.humidity >= 50 && row.humidity <= 70;
            else if (humVal === 'WET') matchHum = row.humidity > 70;
        }

        // 4. Filter Tanggal
        const matchDate = dateVal === '' || rowDateStr === dateVal;

        return matchStatus && matchTemp && matchHum && matchDate;
    });

    currentPage = 1; // Reset to page 1
    renderTable();
};

const deleteLog = async (id) => {
    try {
        const res = await fetch(`${API_BASE}/history/${id}`, { method: 'DELETE' });
        if (res.ok) {
            await fetchHistory();
        } else {
            showNotification("Gagal menghapus data", true);
        }
    } catch (error) {
        console.error("Error deleting log:", error);
    }
};

const exportCSV = () => {
    if (historyData.length === 0) return;
    
    let csvContent = "data:text/csv;charset=utf-8,";
    csvContent += "Timestamp,Suhu,Kelembapan,MQ135_Raw,Gas_Persen,Status_Udara,Buzzer_Aktif,Bahaya_Aktif\n";
    
    historyData.forEach(row => {
        const rowArr = [
            row.timestamp,
            row.temperature,
            row.humidity,
            row.gas_raw,
            row.gas_percentage,
            row.air_status,
            row.buzzer_status,
            row.danger_active
        ];
        csvContent += rowArr.join(",") + "\n";
    });

    const encodedUri = encodeURI(csvContent);
    const link = document.createElement("a");
    link.setAttribute("href", encodedUri);
    link.setAttribute("download", `airguard_history_${new Date().toISOString().split('T')[0]}.csv`);
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
};

const deleteAllLogs = async () => {
    if (confirm("⚠️ PERINGATAN: Apakah Anda yakin ingin menghapus SELURUH riwayat data sensor? Tindakan ini tidak dapat dibatalkan.")) {
        try {
            const res = await fetch(`${API_BASE}/history`, { method: 'DELETE' });
            if (res.ok) {
                await fetchHistory();
                showNotification("Seluruh riwayat data sensor berhasil dikosongkan.");
            } else {
                showNotification("Gagal mengosongkan riwayat data.", true);
            }
        } catch (error) {
            console.error("Error deleting all logs:", error);
        }
    }
};

/* ==========================================================================
   SETTINGS MODULE
   ========================================================================== */
const fetchSettings = async () => {
    try {
        const res = await fetch(`${API_BASE}/settings`);
        if (res.ok) {
            const data = await res.json();
            if (data.thresholds) {
                document.getElementById('threshold_moderate').value = data.thresholds.moderate || 10;
                document.getElementById('threshold_poor').value = data.thresholds.poor || 30;
                document.getElementById('threshold_danger').value = data.thresholds.danger || 60;
            }
            if (data.auto_buzzer !== undefined) {
                document.getElementById('auto_buzzer').checked = data.auto_buzzer === 'true' || data.auto_buzzer === true;
            }
        }
    } catch (error) {
        console.error("Error fetching settings:", error);
    }
};

const setupSettingsForm = () => {
    const form = document.getElementById('settings-form');
    if (!form) return;
    form.addEventListener('submit', async (e) => {
        e.preventDefault();
        
        const payload = {
            threshold_moderate: parseInt(document.getElementById('threshold_moderate').value, 10) || 0,
            threshold_poor: parseInt(document.getElementById('threshold_poor').value, 10) || 0,
            threshold_danger: parseInt(document.getElementById('threshold_danger').value, 10) || 0,
            auto_buzzer: document.getElementById('auto_buzzer').checked
        };

        try {
            const res = await fetch(`${API_BASE}/settings`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload)
            });

            if (res.ok) {
                showNotification("Pengaturan berhasil disimpan.");
            } else {
                const errData = await res.json();
                showNotification(errData.message || "Gagal menyimpan pengaturan.", true);
            }
        } catch (error) {
            console.error("Error saving settings:", error);
            showNotification("Terjadi kesalahan jaringan.", true);
        }
    });
};

/* ==========================================================================
   SPA ROUTER (View Swapping and Animation)
   ========================================================================== */
const switchPage = (pageName) => {
    // 1. Update navigation active state
    document.querySelectorAll('.nav-links a').forEach(link => {
        if (link.getAttribute('data-page') === pageName) {
            link.classList.add('active');
        } else {
            link.classList.remove('active');
        }
    });

    // 2. Transition pages with fade & slide animation
    const currentActiveView = document.querySelector('.page-view.active');
    const targetView = document.getElementById(`view-${pageName}`);

    if (!targetView) return;
    
    // Only perform animation and display swap if views are actually different
    if (currentActiveView !== targetView) {
        if (currentActiveView) {
            // Fade out active view
            currentActiveView.classList.remove('show');
            setTimeout(() => {
                currentActiveView.classList.remove('active');
                
                // Activate new view
                targetView.classList.add('active');
                // Trigger browser repaint to play transition
                requestAnimationFrame(() => {
                    targetView.classList.add('show');
                });
            }, 350); // Matches CSS transition duration
        } else {
            targetView.classList.add('active');
            targetView.classList.add('show');
        }
    }

    // 3. Manage Polling & View Initialization (Always execute this on page swap/load)
    if (pageName === 'dashboard') {
        startDashboardPolling();
    } else {
        stopDashboardPolling();
    }

    if (pageName === 'history') {
        fetchHistory();
    }

    if (pageName === 'settings') {
        fetchSettings();
    }
};

const handleRouting = () => {
    const path = window.location.pathname.replace(/^\/|\/$/g, '');
    if (path === 'history') {
        switchPage('history');
    } else if (path === 'settings') {
        switchPage('settings');
    } else if (path === 'documentation') {
        switchPage('documentation');
    } else {
        switchPage('dashboard');
    }
};

// --- INITIALIZATION ---
document.addEventListener("DOMContentLoaded", () => {
    // Setup Page Event Listeners
    setupControls();
    initChart();
    setupSettingsForm();

    // Initialize browser-side MQTT over WebSocket (works on Vercel)
    initBrowserMQTT();

    // History Listeners
    document.getElementById('status-filter').addEventListener('change', applyFilters);
    document.getElementById('temp-filter').addEventListener('change', applyFilters);
    document.getElementById('hum-filter').addEventListener('change', applyFilters);
    document.getElementById('date-filter').addEventListener('change', applyFilters);
    document.getElementById('btn-export').addEventListener('click', exportCSV);
    document.getElementById('btn-delete-all').addEventListener('click', deleteAllLogs);
    document.getElementById('view-mode-select').addEventListener('change', fetchHistory);

    document.getElementById('btn-prev').addEventListener('click', () => {
        if (currentPage > 1) {
            currentPage--;
            renderTable();
        }
    });

    document.getElementById('btn-next').addEventListener('click', () => {
        const total = filteredData.length;
        if (currentPage * pageSize < total) {
            currentPage++;
            renderTable();
        }
    });

    // Navigation Menu Click Listeners
    document.querySelectorAll('.nav-links a').forEach(link => {
        link.addEventListener('click', (e) => {
            e.preventDefault();
            const page = link.getAttribute('data-page');
            switchPage(page);
            
            // Push URL state
            const newPath = page === 'dashboard' ? '/' : `/${page}`;
            window.history.pushState(null, '', newPath);
        });
    });

    // Handle initial routing and back/forward browser support
    handleRouting();
    window.addEventListener('popstate', handleRouting);
});
