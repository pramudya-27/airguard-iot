import sqlite3
import datetime
import os

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'airguard.db')

def get_connection():
    # check_same_thread=False allows background MQTT thread to use the same connection logic
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    conn = get_connection()
    cursor = conn.cursor()
    
    # Create Sensor Logs table
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS sensor_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT,
            temperature REAL,
            humidity REAL,
            gas_raw INTEGER,
            gas_baseline INTEGER,
            gas_delta INTEGER,
            gas_percentage REAL,
            air_status TEXT,
            buzzer_status BOOLEAN,
            danger_active BOOLEAN,
            rssi INTEGER
        )
    ''')
    
    # Create Settings table
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS settings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            threshold_moderate INTEGER,
            threshold_poor INTEGER,
            threshold_danger INTEGER,
            auto_buzzer BOOLEAN
        )
    ''')
    
    # Insert default settings if empty
    cursor.execute("SELECT COUNT(*) FROM settings")
    if cursor.fetchone()[0] == 0:
        cursor.execute('''
            INSERT INTO settings (threshold_moderate, threshold_poor, threshold_danger, auto_buzzer)
            VALUES (10, 30, 60, 1)
        ''')
        
    conn.commit()
    conn.close()
    print("Database Initialized Successfully.")

def save_telemetry(payload):
    """Save a telemetry payload to the database"""
    conn = get_connection()
    cursor = conn.cursor()
    timestamp = datetime.datetime.now().isoformat()
    
    cursor.execute('''
        INSERT INTO sensor_logs 
        (timestamp, temperature, humidity, gas_raw, gas_baseline, gas_delta, 
         gas_percentage, air_status, buzzer_status, danger_active, rssi)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ''', (
        timestamp,
        payload.get('temperature'),
        payload.get('humidity'),
        payload.get('gas_raw'),
        payload.get('gas_baseline'),
        payload.get('gas_delta'),
        payload.get('gas_percentage'),
        payload.get('air_status'),
        payload.get('buzzer_status'),
        payload.get('danger_active', False),
        payload.get('rssi')
    ))
    conn.commit()
    conn.close()

def save_alert(payload):
    """Save an alert (danger condition). In this simple DB, we just log it as a telemetry point."""
    payload['danger_active'] = True
    save_telemetry(payload)

def get_latest_data():
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute('SELECT * FROM sensor_logs ORDER BY id DESC LIMIT 1')
    row = cursor.fetchone()
    conn.close()
    
    if row:
        # Convert sqlite3.Row to dict
        return dict(row)
    
    # Return empty template if no data
    return {
        "timestamp": None,
        "temperature": None,
        "humidity": None,
        "gas_raw": 0,
        "gas_baseline": 0,
        "gas_delta": 0,
        "gas_percentage": 0,
        "air_status": "UNKNOWN",
        "buzzer_status": False,
        "danger_active": False,
        "rssi": None
    }

def get_history(limit=1000, summarize=True):
    conn = get_connection()
    cursor = conn.cursor()
    if summarize:
        # Group by 1-minute windows (YYYY-MM-DDTHH:MM:00)
        cursor.execute('''
            SELECT 
                -1 as id, -- Dummy ID
                strftime('%Y-%m-%dT%H:%M:00', timestamp) as timestamp,
                AVG(temperature) as temperature,
                AVG(humidity) as humidity,
                ROUND(AVG(gas_raw)) as gas_raw,
                ROUND(AVG(gas_baseline)) as gas_baseline,
                ROUND(AVG(gas_delta)) as gas_delta,
                ROUND(AVG(gas_percentage)) as gas_percentage,
                MAX(buzzer_status) as buzzer_status,
                MAX(danger_active) as danger_active,
                ROUND(AVG(rssi)) as rssi
            FROM sensor_logs
            GROUP BY strftime('%Y-%m-%dT%H:%M:00', timestamp)
            ORDER BY timestamp DESC
            LIMIT ?
        ''', (limit,))
    else:
        cursor.execute('SELECT * FROM sensor_logs ORDER BY id DESC LIMIT ?', (limit,))
    
    rows = cursor.fetchall()
    conn.close()
    
    data = []
    for row in rows:
        d = dict(row)
        # Recalculate air_status based on averaged gas_delta
        delta = d.get('gas_delta', 0)
        if delta < 10:
            d['air_status'] = 'BAIK'
        elif delta < 30:
            d['air_status'] = 'SEDANG'
        elif delta < 60:
            d['air_status'] = 'BURUK'
        else:
            d['air_status'] = 'BAHAYA'
            
        if d.get('danger_active'):
            d['air_status'] = 'BAHAYA'
            
        data.append(d)
        
    return data

def get_settings():
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute('SELECT * FROM settings ORDER BY id DESC LIMIT 1')
    row = cursor.fetchone()
    conn.close()
    
    if row:
        return {
            "thresholds": {
                "moderate": row['threshold_moderate'],
                "poor": row['threshold_poor'],
                "danger": row['threshold_danger']
            },
            "auto_buzzer": bool(row['auto_buzzer'])
        }
    return {}

def save_settings(data):
    conn = get_connection()
    cursor = conn.cursor()
    # Update the single row
    cursor.execute('''
        UPDATE settings 
        SET threshold_moderate = ?, threshold_poor = ?, threshold_danger = ?, auto_buzzer = ?
    ''', (
        data.get('threshold_moderate'),
        data.get('threshold_poor'),
        data.get('threshold_danger'),
        1 if data.get('auto_buzzer') else 0
    ))
    conn.commit()
    conn.close()

def reset_danger_state():
    # If using a separate state table, update it here.
    # For now, it just resets the latest log's danger flag as a quick hack, or nothing.
    pass

def delete_log(log_id):
    """Delete a single log entry by its primary key ID"""
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute('DELETE FROM sensor_logs WHERE id = ?', (log_id,))
    conn.commit()
    conn.close()

def delete_all_logs():
    """Delete all entries in the sensor_logs table"""
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute('DELETE FROM sensor_logs')
    conn.commit()
    conn.close()

