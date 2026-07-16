from flask import Flask, render_template, jsonify, request
from flask_cors import CORS
import os
import threading

import database
import mqtt_service

# Define absolute paths to serve frontend from root project
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE_DIR = os.path.join(BASE_DIR, 'templates')
STATIC_DIR = os.path.join(BASE_DIR, 'static')

app = Flask(__name__, template_folder=TEMPLATE_DIR, static_folder=STATIC_DIR)
CORS(app)

# ====================================================
# WEB TEMPLATE ROUTES
# ====================================================
@app.route('/')
@app.route('/history')
@app.route('/settings')
@app.route('/documentation')
def index():
    return render_template('index.html')

# ====================================================
# DATA API ROUTES
# ====================================================
@app.route('/api/latest', methods=['GET'])
def api_latest():
    """Returns the most recent sensor reading to dashboard (from in-memory cache)"""
    data = mqtt_service.get_latest_telemetry()
    if not data:
        data = database.get_latest_data()
    # Create a copy to prevent in-memory cache mutation
    response_data = dict(data)
    response_data['mqtt_connected'] = mqtt_service.is_connected()
    return jsonify(response_data)

@app.route('/api/status', methods=['GET'])
def api_status():
    """Returns the online/offline status of the ESP8266"""
    status = mqtt_service.get_device_status()
    return jsonify(status)

@app.route('/api/history', methods=['GET', 'DELETE'])
def api_history():
    """Returns or deletes the list of sensor history (grouped per 1-minute by default)"""
    if request.method == 'DELETE':
        database.delete_all_logs()
        return jsonify({"message": "All logs deleted successfully", "status": "success"})
    else:
        # Read the summarize parameter (defaults to true)
        summarize = request.args.get('summarize', 'true').lower() == 'true'
        history_data = database.get_history(summarize=summarize)
        return jsonify(history_data)

@app.route('/api/history/<int:log_id>', methods=['DELETE'])
def api_delete_log(log_id):
    """Deletes a single log entry by its database ID"""
    database.delete_log(log_id)
    return jsonify({"message": f"Log {log_id} deleted successfully", "status": "success"})

@app.route('/api/settings', methods=['GET', 'POST'])
def api_settings():
    """Gets or updates system settings"""
    if request.method == 'POST':
        settings_data = request.json
        # Convert values to correct types (int/bool) before saving and publishing
        cast_settings = {
            "threshold_moderate": int(settings_data.get('threshold_moderate', 10)),
            "threshold_poor": int(settings_data.get('threshold_poor', 30)),
            "threshold_danger": int(settings_data.get('threshold_danger', 60)),
            "auto_buzzer": settings_data.get('auto_buzzer', True) in [True, 'true', 1, '1']
        }
        database.save_settings(cast_settings)
        # Publish settings to MQTT so the device applies them
        mqtt_service.publish_settings(cast_settings)
        return jsonify({"message": "Settings updated successfully", "status": "success"})
    else:
        settings_data = database.get_settings()
        return jsonify(settings_data)

# ====================================================
# DEVICE CONTROL ROUTES
# ====================================================
@app.route('/api/buzzer/test', methods=['POST'])
def api_test_buzzer():
    mqtt_service.publish_command("TEST_BUZZER")
    return jsonify({"message": "Test Buzzer sent", "status": "success"})

@app.route('/api/buzzer/silence', methods=['POST'])
def api_silence_buzzer():
    mqtt_service.publish_command("SILENCE_BUZZER")
    return jsonify({"message": "Silence Buzzer sent", "status": "success"})

@app.route('/api/alarm/reset', methods=['POST'])
def api_reset_alarm():
    mqtt_service.publish_command("RESET_ALARM")
    database.reset_danger_state()
    return jsonify({"message": "Reset Alarm sent", "status": "success"})

@app.route('/api/calibrate', methods=['POST'])
def api_calibrate():
    mqtt_service.publish_command("CALIBRATE_BASELINE")
    return jsonify({"message": "Calibrate command sent", "status": "success"})

# ====================================================
# APPLICATION STARTUP
# ====================================================
if __name__ == '__main__':
    # Initialize the SQLite database
    database.init_db()
    
    # Start MQTT connection in a background thread
    mqtt_thread = threading.Thread(target=mqtt_service.start_mqtt, daemon=True)
    mqtt_thread.start()
    
    # Run Flask server
    app.run(host='0.0.0.0', port=5000, debug=False)
