import sys
import os

# Insert backend directory to system path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'backend'))

# Import the Flask application from backend/app.py
from app import app

if __name__ == '__main__':
    # Default port for Hugging Face is 7860, locally is 5000
    port = int(os.environ.get('PORT', 5000))
    app.run(host='0.0.0.0', port=port)
