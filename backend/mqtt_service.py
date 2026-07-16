import json
import ssl
import paho.mqtt.client as mqtt
import database
import datetime
import time

# --- HiveMQ Cloud Configuration ---
MQTT_HOST = "712b2488276943edbd55e938ca1ba12b.s1.eu.hivemq.cloud"
MQTT_PORT = 8883
MQTT_USER = "mikail"
MQTT_PASS = "mikail123"
DEVICE_ID = "AIRGUARD-01"

# --- Topics ---
TOPIC_TELEMETRY = f"airguard/{DEVICE_ID}/telemetry"
TOPIC_STATUS = f"airguard/{DEVICE_ID}/status"
TOPIC_ALERT = f"airguard/{DEVICE_ID}/alert"
TOPIC_CMD = f"airguard/{DEVICE_ID}/command"
TOPIC_SETTINGS = f"airguard/{DEVICE_ID}/settings"

# Global State
device_status = {
    "status": "OFFLINE",
    "last_seen": None
}

latest_telemetry = {}
last_db_save_time = None

client = None

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("Connected to HiveMQ Cloud successfully.")
        # Subscribe to topics from device
        client.subscribe(TOPIC_TELEMETRY)
        client.subscribe(TOPIC_STATUS)
        client.subscribe(TOPIC_ALERT)
    else:
        print(f"Failed to connect, return code {rc}")

def on_message(client, userdata, msg):
    global latest_telemetry, last_db_save_time
    try:
        payload = json.loads(msg.payload.decode('utf-8'))
        topic = msg.topic
        
        if topic == TOPIC_TELEMETRY:
            # 1. Update in-memory cache for Live 2s updates
            latest_telemetry = {
                "timestamp": datetime.datetime.now().isoformat(),
                "temperature": payload.get('temperature'),
                "humidity": payload.get('humidity'),
                "gas_raw": payload.get('gas_raw'),
                "gas_baseline": payload.get('gas_baseline'),
                "gas_delta": payload.get('gas_delta'),
                "gas_percentage": payload.get('gas_percentage'),
                "air_status": payload.get('air_status'),
                "buzzer_status": payload.get('buzzer_status'),
                "danger_active": payload.get('danger_active', False),
                "rssi": payload.get('rssi')
            }
            
            # 2. Rate-limit saving to Database to every 30 seconds
            current_time = time.time()
            if last_db_save_time is None or (current_time - last_db_save_time) >= 30:
                database.save_telemetry(payload)
                last_db_save_time = current_time
                print("Telemetry saved to database (30s interval)")
                
            # Update last seen status
            device_status['status'] = "ONLINE"
            device_status['last_seen'] = datetime.datetime.now().isoformat()
            
        elif topic == TOPIC_STATUS:
            device_status['status'] = payload.get("status", "UNKNOWN")
            device_status['last_seen'] = datetime.datetime.now().isoformat()
            
        elif topic == TOPIC_ALERT:
            database.save_alert(payload)
            
    except Exception as e:
        print(f"Error processing MQTT message: {e}")

def start_mqtt():
    global client
    client = mqtt.Client(client_id="Flask-Backend", protocol=mqtt.MQTTv311)
    
    # Configure TLS for HiveMQ Cloud
    client.tls_set(tls_version=ssl.PROTOCOL_TLS)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    
    client.on_connect = on_connect
    client.on_message = on_message
    
    try:
        client.connect(MQTT_HOST, MQTT_PORT, 60)
        client.loop_forever()
    except Exception as e:
        print(f"MQTT Connection Error: {e}")

def get_device_status():
    """Returns the current online/offline status of the device."""
    # Snappy timeout check (if no message for 10 seconds -> OFFLINE)
    if device_status['last_seen']:
        last_time = datetime.datetime.fromisoformat(device_status['last_seen'])
        if (datetime.datetime.now() - last_time).total_seconds() > 10:
            device_status['status'] = "OFFLINE"
            
    return device_status

def publish_command(cmd_string):
    """Publish a command to the device."""
    if client and client.is_connected():
        payload = json.dumps({"command": cmd_string})
        client.publish(TOPIC_CMD, payload, qos=1)
        print(f"Published command: {cmd_string}")
    else:
        print("Failed to publish: MQTT not connected")

def publish_settings(settings_dict):
    """Publish new settings to the device."""
    if client and client.is_connected():
        payload = json.dumps(settings_dict)
        client.publish(TOPIC_SETTINGS, payload, qos=1)
        print(f"Published settings: {settings_dict}")
    else:
        print("Failed to publish settings: MQTT not connected")

def is_connected():
    """Checks if Flask backend client is connected to HiveMQ Cloud"""
    return client is not None and client.is_connected()

def get_latest_telemetry():
    """Returns the in-memory latest telemetry dictionary"""
    return latest_telemetry

