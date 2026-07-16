import sys
import os

# Insert backend directory to system path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'backend'))

# Import the Flask application from backend/app.py
from app import app

if __name__ == '__main__':
    # Hugging Face Spaces default port is 7860
    app.run(host='0.0.0.0', port=7860)
