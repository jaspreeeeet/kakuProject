#!/usr/bin/env python3
"""
ESP32 Dashboard Backend Server with AI Vision Analysis
Handles data from ESP32 sensors, stores in database, serves real-time dashboard
and automatically analyzes images with AI-generated captions
"""

from flask import Flask, render_template, jsonify, request, send_from_directory
from flask_cors import CORS
from flask_socketio import SocketIO, join_room, leave_room
import sqlite3
import json
import os
import time
from datetime import datetime
from threading import Thread, Lock
import base64
import hashlib

# AI Vision imports - Google ViT Model (Vision Transformer)
# All imports are optional — server works without them
try:
    from PIL import Image
    import numpy as np
    from transformers import ViTForImageClassification, ViTFeatureExtractor
    AI_AVAILABLE = True
    AI_MODE = "FULL"
    print("✅ Google ViT AI Vision model enabled (FULL mode)")
except ImportError as e:
    print(f"⚠️ Full AI modules not available: {e}")
    try:
        from PIL import Image
        import numpy as np
        AI_AVAILABLE = True
        AI_MODE = "BASIC"
        print("⚠️ Fallback to Basic AI Vision mode (PIL + image analysis)")
    except ImportError as e2:
        print(f"❌ No AI modules available: {e2}")
        AI_AVAILABLE = False
        AI_MODE = "NONE"

# ================= STEP COUNTER STATE =================
from collections import deque
step_counter_lock = Lock()
step_count_global = 0  # Total steps counted
accel_history = deque(maxlen=20)  # Keep last 20 acceleration readings (enough for 2-second span @ 100ms intervals)
last_step_time = 0  # Prevent duplicate step detection
last_walking_time = 0  # Timestamp of most recent detected step (for is_walking flag)
WALKING_ACTIVE_WINDOW = 3  # seconds — pet considered walking if step detected within this window

# 👟 STEP DETECTION PARAMETERS (Optimized for 100ms buffered readings)
# Now with 20 readings per 2 seconds = 100ms intervals = much better temporal resolution
# Can detect faster changes and acceleration peaks more accurately
STEP_DETECTION_THRESHOLD = 0.3  # m/s² - Increased to reduce false positives
STEP_MIN_INTERVAL = 1  # seconds - minimum time between steps (with 100ms reads, can detect ~2 steps/sec)

def detect_steps(accel_x, accel_y, accel_z, current_time):
    """Detect steps from accelerometer data using magnitude changes
    
    Optimized for 100ms buffered readings (20 readings per 2-second batch):
    - Much better temporal resolution = can detect local acceleration peaks
    - Walking produces clear acceleration peaks 0.4-0.8 seconds apart
    - Uses peak detection: when previous reading is higher than both neighbors
    - Can maintain ~2 steps/sec walking pace with 0.5s minimum interval
    """
    global step_count_global, last_step_time
    
    # --- STOSS/BARRIER METHOD (Arduino style) ---
    stoss = (accel_x ** 2) + (accel_y ** 2) + (accel_z ** 2)
    if not hasattr(detect_steps, 'last_step_time'):
        detect_steps.last_step_time = 0
    if not hasattr(detect_steps, 'treshold'):
        detect_steps.treshold = 1  # Default sensitivity level
    steps_detected = 0
    # Lowered barrier for more sensitive step detection
    barrier = 10000  # Set barrier to match sensor data scale
    min_interval = 0.2  # Lowered interval for more frequent step detection
    time_since_last_step = current_time - detect_steps.last_step_time
    if stoss > barrier and time_since_last_step > min_interval:
        steps_detected = 1
        detect_steps.last_step_time = current_time
        global last_walking_time
        last_walking_time = current_time  # Mark when walking was last active
        with step_counter_lock:
            step_count_global += 1
        print(f'     ✅👣 STEP #{step_count_global}! stoss: {stoss:.0f} > barrier: {barrier} | interval: {time_since_last_step:.2f}s')
    return steps_detected

# ================= OTA FIRMWARE UPDATE STATE =================
FIRMWARE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'firmware')
os.makedirs(FIRMWARE_DIR, exist_ok=True)

# In-memory OTA state: device_id → True when OTA is enabled from dashboard
ota_enabled_devices = {}          # {device_id: True/False}
ota_device_auth_tokens = {}       # {device_id: token} — set on first registration
OTA_AUTH_TOKEN = 'kaku-ota-2025'  # Shared secret for device authentication

# OTA live progress state per device (in-memory, resets on server restart)
# {device_id: {ota_status, ota_active, progress, target_version, message, timestamp}}
ota_progress_state = {}  # e.g. {'ESP32_001': {'ota_status': 'downloading', 'progress': 45, ...}}

# ================= CREATE FLASK APP =================
app = Flask(__name__, static_folder='.', static_url_path='')

# ================= BUFFER CONFIGURATION =================
app.config['MAX_CONTENT_LENGTH'] = 50 * 1024 * 1024  # 50MB max request size
app.config['SEND_FILE_MAX_AGE_DEFAULT'] = 0  # Disable caching

# Add stability configurations
import logging
logging.basicConfig(level=logging.ERROR)  # Reduce logging noise
app.logger.setLevel(logging.ERROR)

CORS(app)
socketio = SocketIO(app, 
    cors_allowed_origins="*",
    max_http_buffer_size=10*1024*1024,  # 10MB WebSocket buffer
    ping_timeout=60,
    ping_interval=25,
    logger=False,  # Disable SocketIO logging
    engineio_logger=False  # Disable EngineIO logging
)

# ================= PERFORMANCE OPTIMIZATION =================
@app.after_request
def add_performance_headers(response):
    """Add headers to improve performance and reduce connection exhaustion"""
    # Enable HTTP keep-alive to reuse connections
    response.headers['Connection'] = 'keep-alive'
    response.headers['Keep-Alive'] = 'timeout=30, max=100'
    # Ensure CORS for all responses including errors
    response.headers['Access-Control-Allow-Origin'] = '*'
    response.headers['Access-Control-Allow-Headers'] = 'Content-Type,Authorization'
    response.headers['Access-Control-Allow-Methods'] = 'GET,PUT,POST,DELETE,OPTIONS'
    
    # Reduce overhead
    if 'Content-Type' not in response.headers:
        response.headers['Content-Type'] = 'application/json'
    return response

@app.errorhandler(Exception)
def handle_exception(e):
    """Global error handler for all unhandled exceptions"""
    # Log the full traceback
    import traceback
    error_details = traceback.format_exc()
    print(f"🔥 CRITICAL ERROR: {str(e)}")
    print(error_details)
    
    # Create JSON response
    response = jsonify({
        "status": "error",
        "message": str(e),
        "type": e.__class__.__name__,
        "traceback": error_details if app.debug else "Enable debug mode for full traceback"
    })
    response.status_code = 500
    return response

@app.route('/api/health')
def health_check():
    """Health check endpoint for diagnostics"""
    return jsonify({
        "status": "online",
        "timestamp": datetime.now().isoformat(),
        "project": "kakuProject",
        "endpoints": ["/api/latest-image", "/api/step-counter/get", "/api/sensor-data"]
    }), 200

# Database configuration
DB_PATH = 'sensor_data.db'
ALLOWED_EXTENSIONS = {'jpg', 'jpeg', 'png', 'gif'}

# AI Configuration
if AI_AVAILABLE:
    CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".cache", "huggingface")
    os.environ['HUGGINGFACE_HUB_CACHE'] = CACHE_DIR
    os.makedirs(CACHE_DIR, exist_ok=True)
    AI_DEVICE = -1  # CPU only on Cloud Run
    print("⚠️ Using CPU for AI analysis")
    ai_classifier = None  # Lazy loaded on first use

# ================= ORIENTATION DETECTION (Server-side) =================
def detect_device_orientation(ax, ay, az):
    """
    Detect device orientation from raw accelerometer data
    This computation is now done on the server (not on ESP32)
    
    Returns: (direction_string, confidence_percentage)
    """
    try:
        # Normalize accelerometer values (remove gravity bias)
        magnitude = (ax**2 + ay**2 + az**2) ** 0.5
        
        # Calculate confidence based on how close to 1g the total acceleration is
        # 1g = 9.81 m/s² (gravity only, device not accelerating)
        confidence = 100.0 - abs(magnitude - 9.81) * 10.0
        if confidence > 100.0:
            confidence = 100.0
        if confidence < 0.0:
            confidence = 0.0
        
        # Determine dominant axis and direction
        abs_ax = abs(ax)
        abs_ay = abs(ay)
        abs_az = abs(az)
        
        # Z-axis dominant (device flat or inverted)
        if abs_az > abs_ax and abs_az > abs_ay:
            if az > 7.0:
                return "NEUTRAL", confidence      # Device flat, Z pointing up
            if az < -7.0:
                return "INVERTED", confidence     # Device flipped, Z pointing down
        
        # X-axis dominant (device tilted left/right)
        if abs_ax > abs_ay and abs_ax > abs_az:
            if ax > 5.0:
                return "RIGHT", confidence        # Device tilted right
            if ax < -5.0:
                return "LEFT", confidence         # Device tilted left
        
        # Y-axis dominant (device tilted forward/back)
        if abs_ay > abs_ax and abs_ay > abs_az:
            if ay > 5.0:
                return "BACK", confidence         # Device tilted back
            if ay < -5.0:
                return "FORWARD", confidence      # Device tilted forward
        
        return "NEUTRAL", confidence              # Default fallback
    
    except Exception as e:
        print(f"❌ Error in orientation detection: {e}")
        return "UNKNOWN", 0.0

# ViT AI Model components (lazy loaded)
vit_extractor = None
vit_model = None

def analyze_image_with_ai(image_data, is_path=False):
    """Analyze image using Google ViT model or basic fallback"""
    global vit_extractor, vit_model
    
    if not AI_AVAILABLE:
        return "AI analysis not available"
    
    try:
        if AI_MODE == "FULL":
            if vit_model is None:
                print("Loading Google ViT model...")
                vit_extractor = ViTFeatureExtractor.from_pretrained("google/vit-base-patch16-224")
                vit_model = ViTForImageClassification.from_pretrained("google/vit-base-patch16-224")
                print("✅ Google ViT model loaded successfully")
            
            # Load image from path or binary data
            if is_path:
                image = Image.open(image_data).convert('RGB')
            else:
                import io
                image = Image.open(io.BytesIO(image_data)).convert('RGB')
            
            # Classify image
            inputs = vit_extractor(images=image, return_tensors="pt")
            outputs = vit_model(**inputs)
            logits = outputs.logits
            predicted_class_idx = logits.argmax(-1).item()
            label = vit_model.config.id2label[predicted_class_idx]
            
            print(f"🤖 ViT Classification: {label}")
            return label.replace('_', ' ').capitalize()
            
        else:
            # Basic fallback — just acknowledge the image
            return "Image received"
            
    except Exception as e:
        print(f"AI Analysis error: {e}")
        import traceback
        traceback.print_exc()
        return f"Analysis failed: {str(e)[:80]}"

# ================= BACKGROUND AI ANALYSIS (ENABLED) =================
def analyze_and_store_image_blob(reading_id, image_blob):
    """Background task: Run ViT analysis and store result"""
    try:
        print(f"🤖 [BACKGROUND] Starting ViT analysis for reading #{reading_id}...")
        ai_caption = analyze_image_with_ai(image_blob, is_path=False)
        
        # Store in database
        with db_lock:
            conn = get_db_connection()
            if conn:
                try:
                    cursor = conn.cursor()
                    cursor.execute('''
                        UPDATE sensor_readings 
                        SET ai_caption = ?
                        WHERE id = ?
                    ''', (ai_caption, reading_id))
                    conn.commit()
                    print(f"✅ [BG] Stored BLIP caption for reading #{reading_id}")
                except sqlite3.Error as e:
                    print(f"❌ [BG] DB Error: {e}")
                finally:
                    conn.close()
    except Exception as e:
        print(f"❌ [BG] Analysis failed: {e}")

def sync_pet_state_to_db(device_id, pet_state):
    """Update mirrored pet state in database using hardware-authoritative values"""
    with db_lock:
        conn = get_db_connection()
        if not conn: return
        try:
            cursor = conn.cursor()
            cursor.execute('''
                INSERT INTO pet_state (
                    device_id, age, level, xp, uptime, 
                    health, hunger, thirst, happiness, energy, discipline,
                    is_sick, has_poop, updated_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)
                ON CONFLICT(id) DO UPDATE SET
                    age=excluded.age, level=excluded.level, xp=excluded.xp, uptime=excluded.uptime,
                    health=excluded.health, hunger=excluded.hunger, thirst=excluded.thirst,
                    happiness=excluded.happiness, energy=excluded.energy, discipline=excluded.discipline,
                    is_sick=excluded.is_sick, has_poop=excluded.has_poop, updated_at=CURRENT_TIMESTAMP
                WHERE device_id = ?
            ''', (
                device_id, pet_state.get('age', 0), pet_state.get('level', 1), 
                pet_state.get('xp', 0), pet_state.get('uptime', 0),
                pet_state.get('health', 100), pet_state.get('hunger', 0), 
                pet_state.get('thirst', 0), pet_state.get('happiness', 100), 
                pet_state.get('energy', 100), pet_state.get('discipline', 100),
                pet_state.get('is_sick', 0), pet_state.get('has_poop', 0), device_id
            ))
            
            # Legacy simple update for single-row setup
            cursor.execute('''
                UPDATE pet_state SET 
                    age=?, level=?, xp=?, uptime=?, 
                    health=?, hunger=?, thirst=?, happiness=?, energy=?, discipline=?,
                    is_sick=?, has_poop=?, updated_at=CURRENT_TIMESTAMP
                WHERE device_id = ? OR id = (SELECT MIN(id) FROM pet_state)
            ''', (
                pet_state.get('age', 0), pet_state.get('level', 1), 
                pet_state.get('xp', 0), pet_state.get('uptime', 0),
                pet_state.get('health', 100), pet_state.get('hunger', 0), 
                pet_state.get('thirst', 0), pet_state.get('happiness', 100), 
                pet_state.get('energy', 100), pet_state.get('discipline', 100),
                pet_state.get('is_sick', 0), pet_state.get('has_poop', 0), device_id
            ))
            conn.commit()
            
            # 📢 Broadcast updated pet state to all connected clients for real-time dashboard updates
            try:
                def emit_sync_update():
                    with app.app_context():
                        socketio.emit('pet_state_update', {
                            'device_id': device_id,
                            'age': pet_state.get('age', 0),
                            'level': pet_state.get('level', 1),
                            'xp': pet_state.get('xp', 0),
                            'health': pet_state.get('health', 100),
                            'hunger': pet_state.get('hunger', 0),
                            'thirst': pet_state.get('thirst', 0),
                            'happiness': pet_state.get('happiness', 100),
                            'energy': pet_state.get('energy', 100),
                            'discipline': pet_state.get('discipline', 100),
                            'has_poop': bool(pet_state.get('has_poop', 0)),
                            'is_sick': bool(pet_state.get('is_sick', 0)),
                            'current_menu': pet_state.get('current_menu', 'MAIN'),
                            'uptime': pet_state.get('uptime', 0),
                            'timestamp': datetime.now().isoformat()
                        })
                socketio.start_background_task(emit_sync_update)
            except Exception as socket_err:
                print(f"⚠️ Socket broadcast failed during sync: {socket_err}")

        except sqlite3.Error as e:
            print(f"❌ Pet Sync Error: {e}")
        finally:
            conn.close()
#                     print(f"❌ [BG] Database error: {e}")
#                 finally:
#                     conn.close()
#         
#         # Broadcast result to dashboard
#         # broadcast_camera_update(...)
#         
#         print(f"✅ [BG] AI complete: {ai_caption[:60]}...")
#     
#     except Exception as e:
#         print(f"❌ [BG] AI analysis error: {e}")

# Helper function for broadcasting camera updates to all connected clients
def broadcast_camera_update(image_id=None, ai_caption=None, timestamp=None):
    """
    Broadcast camera update event to all connected WebSocket clients
    Uses database image ID to fetch base64 image data
    """
    def emit_update():
        with app.app_context():
            try:
                # ✅ Fetch image from database and convert to base64
                if image_id:
                    conn = sqlite3.connect(DB_PATH)
                    cursor = conn.cursor()
                    cursor.execute('SELECT camera_image FROM sensor_readings WHERE id = ?', (image_id,))
                    result = cursor.fetchone()
                    conn.close()
                    
                    if result and result[0]:
                        image_binary = result[0]
                        image_base64 = base64.b64encode(image_binary).decode('utf-8')
                        image_url = f'data:image/jpeg;base64,{image_base64}'
                    else:
                        print(f"⚠️ No image found for id={image_id}")
                        return
                else:
                    print("⚠️ No image_id provided to broadcast")
                    return
                
                socketio.emit('camera_update', {
                    'image_url': image_url,
                    'ai_caption': ai_caption,
                    'timestamp': timestamp or datetime.now().isoformat(),
                    'device_id': 'ESP32_CAM',
                    'image_id': image_id,
                    'source': 'database'
                })
                print(f"📡 Broadcasted camera update to all connected clients (id={image_id})")
            except Exception as e:
                print(f"❌ Error broadcasting camera update: {e}")
    
    socketio.start_background_task(emit_update)

def broadcast_step_counter_update(total_steps, daily_steps=0):
    """
    Broadcast step counter update event to all connected WebSocket clients
    Uses a background task to ensure proper context for emission
    """
    def emit_update():
        with app.app_context():
            socketio.emit('step_counter_updated', {
                'total_steps': total_steps,
                'daily_steps': daily_steps,
                'timestamp': datetime.now().isoformat(),
                'device_id': 'ESP32_001'
            })
            print(f"👟 Broadcasted step update: {total_steps} steps to all connected clients")
    
    socketio.start_background_task(emit_update)

# Endpoint to fetch image from database by sensor_readings id
from flask import Response
@app.route('/api/image/<int:image_id>')
def get_image(image_id):
    with db_lock:
        conn = get_db_connection()
        if conn:
            cursor = conn.cursor()
            cursor.execute('SELECT camera_image FROM sensor_readings WHERE id=?', (image_id,))
            row = cursor.fetchone()
            conn.close()
            if row and row[0]:
                return Response(row[0], mimetype='image/jpeg')
    return jsonify({'error': 'Image not found'}), 404

# Thread-safe database helper
db_lock = Lock()

def get_db_connection():
    """Create a thread-safe database connection"""
    try:
        # Check if DB_PATH exists, if not, it will be created by sqlite3.connect
        if not os.path.exists(DB_PATH):
            print(f"⚠️ Database not found at {DB_PATH}, will be created.")
            
        conn = sqlite3.connect(DB_PATH, timeout=20)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA journal_mode=WAL;")
        conn.execute("PRAGMA synchronous=NORMAL;")
        conn.execute("PRAGMA cache_size=10000;")
        conn.execute("PRAGMA temp_store=memory;")
        return conn
    except Exception as e:
        print(f"❌ DATABASE CONNECTION ERROR: {e}")
        return None

# Initialize database
def init_database():
    """Initialize database with proper error handling"""
    with db_lock:
        conn = get_db_connection()
        if not conn:
            print("❌ Failed to initialize database")
            return False
        
        try:
            cursor = conn.cursor()
            
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS sensor_readings (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT DEFAULT 'ESP32_001',
                    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
                    accel_x REAL,
                    accel_y REAL,
                    accel_z REAL,
                    gyro_x REAL,
                    gyro_y REAL,
                    gyro_z REAL,
                    mic_level REAL,
                    sound_data INTEGER,
                    camera_image BLOB,
                    audio_data BLOB,
                    image_filename TEXT,
                    ai_caption TEXT,
                    device_orientation TEXT,
                    orientation_confidence REAL,
                    calibrated_ax REAL,
                    calibrated_ay REAL,
                    calibrated_az REAL,
                    chip_temperature REAL
                )
            ''')
            
            # Add new columns if they don't exist (for existing databases)
            cursor.execute("PRAGMA table_info(sensor_readings)")
            columns = [column[1] for column in cursor.fetchall()]
            
            if 'image_filename' not in columns:
                cursor.execute("ALTER TABLE sensor_readings ADD COLUMN image_filename TEXT")
                print("✅ Added image_filename column")
                
            if 'ai_caption' not in columns:
                cursor.execute("ALTER TABLE sensor_readings ADD COLUMN ai_caption TEXT")
                print("✅ Added ai_caption column for AI analysis")
                
            if 'device_orientation' not in columns:
                cursor.execute("ALTER TABLE sensor_readings ADD COLUMN device_orientation TEXT")
                print("✅ Added device_orientation column")
                
            if 'orientation_confidence' not in columns:
                cursor.execute("ALTER TABLE sensor_readings ADD COLUMN orientation_confidence REAL")
                print("✅ Added orientation_confidence column")
                
            if 'calibrated_ax' not in columns:
                cursor.execute("ALTER TABLE sensor_readings ADD COLUMN calibrated_ax REAL")
                print("✅ Added calibrated_ax column")
                
            if 'calibrated_ay' not in columns:
                cursor.execute("ALTER TABLE sensor_readings ADD COLUMN calibrated_ay REAL")
                print("✅ Added calibrated_ay column")
                
            if 'calibrated_az' not in columns:
                cursor.execute("ALTER TABLE sensor_readings ADD COLUMN calibrated_az REAL")
                print("✅ Added calibrated_az column")
            
            # Add step_count column for step tracking
            cursor.execute("PRAGMA table_info(sensor_readings)")
            columns = [column[1] for column in cursor.fetchall()]
            if 'step_count' not in columns:
                cursor.execute("ALTER TABLE sensor_readings ADD COLUMN step_count INTEGER DEFAULT 0")
                print("✅ Added step_count column")
            
            # Add chip_temperature column for ESP32 internal temperature
            if 'chip_temperature' not in columns:
                cursor.execute("ALTER TABLE sensor_readings ADD COLUMN chip_temperature REAL")
                print("✅ Added chip_temperature column")
            
            # Create step_statistics table for aggregated step data
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS step_statistics (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT DEFAULT 'ESP32_001',
                    date_recorded DATE DEFAULT CURRENT_DATE,
                    total_steps INTEGER DEFAULT 0,
                    peak_steps INTEGER DEFAULT 0,
                    avg_step_interval REAL DEFAULT 0.0,
                    activity_level TEXT DEFAULT 'INACTIVE',
                    recorded_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
                )
            ''')
            
            # Add columns if they don't exist
            cursor.execute("PRAGMA table_info(step_statistics)")
            stat_columns = [column[1] for column in cursor.fetchall()]
            
            if 'peak_steps' not in stat_columns:
                cursor.execute("ALTER TABLE step_statistics ADD COLUMN peak_steps INTEGER DEFAULT 0")
                print("✅ Added peak_steps column to step_statistics")
            
            if 'avg_step_interval' not in stat_columns:
                cursor.execute("ALTER TABLE step_statistics ADD COLUMN avg_step_interval REAL DEFAULT 0.0")
                print("✅ Added avg_step_interval column")
            
            if 'activity_level' not in stat_columns:
                cursor.execute("ALTER TABLE step_statistics ADD COLUMN activity_level TEXT DEFAULT 'INACTIVE'")
                print("✅ Added activity_level column")
            
            if 'updated_at' not in stat_columns:
                cursor.execute("ALTER TABLE step_statistics ADD COLUMN updated_at DATETIME DEFAULT CURRENT_TIMESTAMP")
                print("✅ Added updated_at column")
            
            print("✅ Created step_statistics table")            
            
            # Add device_id column if it doesn't exist (for existing databases)
            cursor.execute("PRAGMA table_info(sensor_readings)")
            columns = [column[1] for column in cursor.fetchall()]
            if 'device_id' not in columns:
                cursor.execute("ALTER TABLE sensor_readings ADD COLUMN device_id TEXT DEFAULT 'ESP32_001'")
                print("✅ Added device_id column to existing sensor_readings table")
            
            # Create important_events table for ESP32 event polling
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS important_events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT DEFAULT 'ESP32_001',
                    event_type TEXT NOT NULL,
                    message TEXT NOT NULL, 
                    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                    is_sent BOOLEAN DEFAULT 0
                )
            ''')
            
            # Create oled_display_state table for display commands
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS oled_display_state (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT DEFAULT 'ESP32_001',
                    animation_type TEXT DEFAULT 'pet',
                    animation_id INTEGER DEFAULT 1,
                    animation_name TEXT DEFAULT 'CHILD',
                    show_home_icon BOOLEAN DEFAULT 0,
                    show_food_icon BOOLEAN DEFAULT 0,
                    show_poop_icon BOOLEAN DEFAULT 0,
                    screen_type TEXT DEFAULT 'MAIN',
                    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                    updated_by TEXT DEFAULT 'web_ui'
                )
            ''')
            
            # ===== DATABASE MIGRATION: Add missing columns to existing tables =====
            # Check and add show_home_icon column if it doesn't exist
            try:
                cursor.execute('PRAGMA table_info(oled_display_state)')
                columns = [column[1] for column in cursor.fetchall()]
                
                if 'show_home_icon' not in columns:
                    cursor.execute('ALTER TABLE oled_display_state ADD COLUMN show_home_icon BOOLEAN DEFAULT 0')
                    print("✅ Added show_home_icon column to oled_display_state")
                
                if 'show_food_icon' not in columns:
                    cursor.execute('ALTER TABLE oled_display_state ADD COLUMN show_food_icon BOOLEAN DEFAULT 0')
                    print("✅ Added show_food_icon column to oled_display_state")
                
                if 'show_poop_icon' not in columns:
                    cursor.execute('ALTER TABLE oled_display_state ADD COLUMN show_poop_icon BOOLEAN DEFAULT 0')
                    print("✅ Added show_poop_icon column to oled_display_state")
                
                if 'screen_type' not in columns:
                    cursor.execute('ALTER TABLE oled_display_state ADD COLUMN screen_type TEXT DEFAULT "MAIN"')
                    print("✅ Added screen_type column to oled_display_state")
            except Exception as e:
                print(f"⚠️ Migration warning: {e}")
            
            # Initialize default OLED state if not exists
            cursor.execute('SELECT COUNT(*) FROM oled_display_state')
            if cursor.fetchone()[0] == 0:
                cursor.execute('''
                    INSERT INTO oled_display_state 
                    (device_id, animation_type, animation_id, animation_name, updated_by)
                    VALUES (?, ?, ?, ?, ?)
                ''', ('ESP32_001', 'pet', 1, 'CHILD', 'system_init'))
                print("✅ Initialized default OLED display state in database")
            
            # 🐾 Create pet_state table (Authoritative Mirror for ESP32)
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS pet_state (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT DEFAULT 'ESP32_001',
                    
                    age INTEGER DEFAULT 0,
                    level INTEGER DEFAULT 1,
                    xp INTEGER DEFAULT 0,
                    uptime INTEGER DEFAULT 0,
                    
                    health INTEGER DEFAULT 100,
                    hunger INTEGER DEFAULT 0,
                    thirst INTEGER DEFAULT 0,
                    happiness INTEGER DEFAULT 100,
                    energy INTEGER DEFAULT 100,
                    discipline INTEGER DEFAULT 100,
                    
                    is_sick BOOLEAN DEFAULT 0,
                    has_poop BOOLEAN DEFAULT 0,
                    
                    current_menu TEXT DEFAULT 'MAIN',
                    current_emotion TEXT DEFAULT 'IDLE',
                    
                    version INTEGER DEFAULT 0,
                    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
                )
            ''')
            
            # Ensure new columns exist for existing databases
            try:
                cursor.execute('ALTER TABLE pet_state ADD COLUMN level INTEGER DEFAULT 1')
                cursor.execute('ALTER TABLE pet_state ADD COLUMN xp INTEGER DEFAULT 0')
                cursor.execute('ALTER TABLE pet_state ADD COLUMN thirst INTEGER DEFAULT 0')
                cursor.execute('ALTER TABLE pet_state ADD COLUMN uptime INTEGER DEFAULT 0')
                cursor.execute('ALTER TABLE pet_state ADD COLUMN is_sick BOOLEAN DEFAULT 0')
                cursor.execute('ALTER TABLE pet_state ADD COLUMN has_poop BOOLEAN DEFAULT 0')
            except sqlite3.OperationalError:
                pass # Columns already exist
                
            print("✅ Created/Updated pet_state table")
            
            # Game rewards table for catch-food game
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS game_rewards (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT DEFAULT 'ESP32_001',
                    game_type TEXT DEFAULT 'CATCH_FOOD',
                    score INTEGER DEFAULT 0,
                    kakucoin INTEGER DEFAULT 0,
                    play_duration INTEGER DEFAULT 0,
                    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
                )
            ''')
            print("✅ Created game_rewards table")
            
            # Firmware versions table for OTA updates
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS firmware_versions (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    version TEXT NOT NULL,
                    filename TEXT NOT NULL,
                    file_size INTEGER DEFAULT 0,
                    checksum TEXT DEFAULT '',
                    uploaded_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                    notes TEXT DEFAULT ''
                )
            ''')
            print("✅ Created firmware_versions table")
            
            # ===== DATABASE MIGRATION: Add missing columns to pet_state =====
            # Must run AFTER CREATE TABLE pet_state so table is guaranteed to exist
            cursor.execute('PRAGMA table_info(pet_state)')
            pet_columns = [column[1] for column in cursor.fetchall()]
            
            # NOTE: SQLite ALTER TABLE ADD COLUMN only allows constant defaults
            # CURRENT_TIMESTAMP is NOT a constant — use NULL instead
            migration_cols = [
                ('last_hunger_update', 'DATETIME DEFAULT NULL'),
                ('sick_pending',       'BOOLEAN DEFAULT 0'),
                ('discipline',         'INTEGER DEFAULT 100'),
            ]
            for col_name, col_def in migration_cols:
                try:
                    if col_name not in pet_columns:
                        cursor.execute(f'ALTER TABLE pet_state ADD COLUMN {col_name} {col_def}')
                        print(f"✅ Added {col_name} column to pet_state")
                except Exception as e:
                    print(f"⚠️ Migration {col_name}: {e}")
            
            conn.commit()  # Explicit commit after migrations
            print("✅ pet_state migrations complete")
            
            # Initialize one pet_state row if not exists
            cursor.execute('SELECT COUNT(*) FROM pet_state')
            if cursor.fetchone()[0] == 0:
                cursor.execute('''
                    INSERT INTO pet_state 
                    (device_id, age, stage, health, hunger, cleanliness, happiness, energy,
                     current_menu, current_emotion, last_age_increment)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)
                ''', ('ESP32_001', 0, 'INFANT', 100, 0, 100, 100, 100, 'MAIN', 'IDLE'))
                print("✅ Initialized default pet state in database")
            
            conn.commit()
            print("✅ Database initialized successfully")
            return True
        except sqlite3.Error as e:
            print(f"❌ Database initialization error: {e}")
            return False
        finally:
            conn.close()

# Initialize database on startup
init_database()

# ===== EMERGENCY DB MIGRATION ENDPOINT =====
@app.route('/api/db-migrate', methods=['GET'])
def emergency_db_migrate():
    """Emergency endpoint to run DB migrations on-demand.
    Use when sick_pending / discipline / last_hunger_update columns are missing.
    Call: GET /api/db-migrate
    """
    results = []
    try:
        conn = get_db_connection()
        if not conn:
            return jsonify({'status': 'error', 'message': 'DB connection failed'}), 500
        cursor = conn.cursor()
        
        cursor.execute('PRAGMA table_info(pet_state)')
        pet_columns = [col[1] for col in cursor.fetchall()]
        
        # NOTE: SQLite ALTER TABLE ADD COLUMN only allows constant defaults
        # CURRENT_TIMESTAMP not allowed — use NULL instead
        for col, definition in [
            ('last_hunger_update', 'DATETIME DEFAULT NULL'),
            ('sick_pending',       'BOOLEAN DEFAULT 0'),
            ('discipline',         'INTEGER DEFAULT 100'),
        ]:
            if col not in pet_columns:
                try:
                    cursor.execute(f'ALTER TABLE pet_state ADD COLUMN {col} {definition}')
                    results.append(f'✅ Added {col}')
                except Exception as col_err:
                    results.append(f'❌ {col}: {str(col_err)}')
            else:
                results.append(f'⏭ {col} already exists')
        
        conn.commit()
        conn.close()
        return jsonify({'status': 'success', 'migrations': results}), 200
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 500

# ==================== PET ENGINE - CENTRAL UPDATE FUNCTION ====================

def update_pet_state_atomic(device_id, update_fields: dict):
    """
    Thread-safe atomic update of pet state with version control
    Prevents race conditions and lost updates
    
    Args:
        device_id: Device identifier
        update_fields: Dict of fields to update
    
    Returns:
        Updated pet state dict or None on error
    """
    with db_lock:
        conn = get_db_connection()
        if not conn:
            return None
        
        try:
            cursor = conn.cursor()
            
            # Fetch current state with version — use SELECT * for schema resilience
            cursor.execute('SELECT * FROM pet_state WHERE device_id = ?', (device_id,))
            
            result = cursor.fetchone()
            if not result:
                print(f"❌ No pet state found for {device_id}")
                return None
            
            # Build current state dynamically from column names
            col_names = [desc[0] for desc in cursor.description]
            row = dict(zip(col_names, result))
            
            current_state = {
                'id': row.get('id'),
                'version': row.get('version', 0),
                'action_lock': row.get('action_lock', 0),
                'emotion_expire_at': row.get('emotion_expire_at'),
                'age': row.get('age', 0),
                'stage': row.get('stage', 'INFANT'),
                'health': row.get('health', 100),
                'hunger': row.get('hunger', 0),
                'cleanliness': row.get('cleanliness', 100),
                'happiness': row.get('happiness', 100),
                'energy': row.get('energy', 100),
                'poop_present': row.get('poop_present', 0),
                'poop_timestamp': row.get('poop_timestamp'),
                'digestion_due_time': row.get('digestion_due_time'),
                'current_menu': row.get('current_menu', 'MAIN'),
                'current_emotion': row.get('current_emotion', 'IDLE'),
                'last_feed_time': row.get('last_feed_time'),
                'last_play_time': row.get('last_play_time'),
                'last_sleep_time': row.get('last_sleep_time'),
                'last_clean_time': row.get('last_clean_time'),
                'last_age_increment': row.get('last_age_increment'),
                'last_hunger_update': row.get('last_hunger_update'),
                'sick_pending': row.get('sick_pending', 0) or 0,
                'discipline': row.get('discipline', 100) or 100,
            }
            
            # Merge updates
            new_state = {**current_state, **update_fields}
            new_state['version'] = current_state['version'] + 1
            new_state['updated_at'] = datetime.now().isoformat()
            
            # Build UPDATE dynamically based on which columns exist in the DB
            cursor.execute('PRAGMA table_info(pet_state)')
            existing_cols = {col[1] for col in cursor.fetchall()}
            
            # Core columns always present
            update_pairs = [
                ('version', new_state['version']),
                ('age', new_state['age']),
                ('stage', new_state['stage']),
                ('health', new_state['health']),
                ('hunger', new_state['hunger']),
                ('cleanliness', new_state['cleanliness']),
                ('happiness', new_state['happiness']),
                ('energy', new_state['energy']),
                ('poop_present', new_state['poop_present']),
                ('poop_timestamp', new_state['poop_timestamp']),
                ('digestion_due_time', new_state['digestion_due_time']),
                ('current_menu', new_state['current_menu']),
                ('current_emotion', new_state['current_emotion']),
                ('emotion_expire_at', new_state['emotion_expire_at']),
                ('action_lock', new_state['action_lock']),
                ('last_feed_time', new_state['last_feed_time']),
                ('last_play_time', new_state['last_play_time']),
                ('last_sleep_time', new_state['last_sleep_time']),
                ('last_clean_time', new_state['last_clean_time']),
                ('last_age_increment', new_state['last_age_increment']),
            ]
            
            # Optional columns — only include if they exist in DB
            for opt_col, default in [('last_hunger_update', None), ('sick_pending', 0), ('discipline', 100)]:
                if opt_col in existing_cols:
                    update_pairs.append((opt_col, new_state.get(opt_col, default)))
            
            set_clause = ', '.join(f'{col} = ?' for col, _ in update_pairs) + ', updated_at = CURRENT_TIMESTAMP'
            values = [val for _, val in update_pairs]
            values.extend([current_state['id'], current_state['version']])
            
            cursor.execute(
                f'UPDATE pet_state SET {set_clause} WHERE id = ? AND version = ?',
                values
            )
            
            if cursor.rowcount == 0:
                print("⚠️ Version conflict detected, retrying...")
                conn.close()
                return update_pet_state_atomic(device_id, update_fields)
            
            conn.commit()
            print(f"✅ Pet state updated atomically (version {current_state['version']} → {new_state['version']})")
            return new_state
            
        except Exception as e:
            print(f"❌ Error updating pet state: {e}")
            return None
        finally:
            conn.close()

def get_pet_state(device_id='ESP32_001'):
    """Get current pet state safely — handles missing columns gracefully"""
    with db_lock:
        conn = get_db_connection()
        if not conn:
            return None
        
        try:
            cursor = conn.cursor()
            cursor.execute('SELECT * FROM pet_state WHERE device_id = ?', (device_id,))
            
            result = cursor.fetchone()
            if not result:
                return None
            
            # Build column name → value map dynamically
            col_names = [desc[0] for desc in cursor.description]
            row = dict(zip(col_names, result))
            
            state = {
                'age': row.get('age', 0),
                'level': row.get('level', 1),
                'xp': row.get('xp', 0),
                'health': row.get('health', 100),
                'hunger': row.get('hunger', 0),
                'thirst': row.get('thirst', 0),
                'happiness': row.get('happiness', 100),
                'energy': row.get('energy', 100),
                'discipline': row.get('discipline', 100),
                'is_sick': bool(row.get('is_sick', 0)),
                'has_poop': bool(row.get('has_poop', 0)),
                'uptime': row.get('uptime', 0),
                'current_menu': row.get('current_menu', 'MAIN'),
                'current_emotion': row.get('current_emotion', 'IDLE'),
                'emotion_expire_at': row.get('emotion_expire_at'),
            }
            
            # Derive stage from age (matching hardware logic)
            age = state['age']
            if age < 3: state['stage'] = 'INFANT'
            elif age < 10: state['stage'] = 'CHILD'
            elif age < 25: state['stage'] = 'ADULT'
            else: state['stage'] = 'OLD'
            
            return state
        finally:
            conn.close()

def get_emotion_priority(state):
    """
    Return highest priority emotion based on pet state
    Priority: SICK > POOP > HUNGER > PLAY > SLEEP > IDLE
    """
    # Check if emotion is locked (temporary emotion active)
    if state.get('emotion_expire_at'):
        from datetime import datetime
        expire_time = datetime.fromisoformat(state['emotion_expire_at']) if isinstance(state['emotion_expire_at'], str) else state['emotion_expire_at']
        if expire_time and expire_time > datetime.now():
            return state['current_emotion']  # Keep locked emotion
    
    # Priority-based emotion selection
    if state.get('is_sick'):
        return 'SICK'
    
    if state.get('has_poop'):
        return 'POOP'
    
    if state['hunger'] > 70:
        if state['stage'] == 'INFANT':
            return 'CRY'
        return 'HUNGER'
    
    if state['energy'] < 30:
        return 'SLEEP'
    
    # Default states
    if state['happiness'] > 80:
        return 'HAPPY'
    elif state['happiness'] < 40:
        return 'SAD'
    
    return 'IDLE'

# ==================== PET ENGINE BACKGROUND THREAD ====================

def pet_engine_cycle():
    """
    ⚠️ LEGACY - Server-side pet logic is now DISABLED.
    The ESP32 Hardware is now the source of truth for all physiological state.
    """
    pass
    # [LEGACY CODE COMMENTED OUT TO PREVENT LINTS]
    # while True:
    #     try:
    #         time.sleep(60)
    #         device_id = 'ESP32_001'
    #         ...
    #         
    #         # Apply updates atomically
    #         if updates:
    #             result = update_pet_state_atomic(device_id, updates)
    #             if result:
    #                 print(f"✅ Pet engine cycle complete")
    #                 
    #                 # Broadcast update to frontend
    #                 socketio.start_background_task(lambda: socketio.emit('pet_state_update', {
    #                     'stage': result['stage'],
    #                     'emotion': result['current_emotion'],
    #                     'health': result['health'],
    #                     'hunger': result['hunger'],
    #                     'cleanliness': result['cleanliness'],
    #                     'happiness': result['happiness'],
    #                     'energy': result['energy'],
    #                     'poop_present': result['poop_present'],
    #                     'age': result['age']
    #                 }))
    #             else:
    #                 print("❌ Pet engine update failed")
    #         else:
    #             print("No updates needed")
    #                 
    #     except Exception as e:
    #         print(f"❌ Pet engine error: {e}")
    #         import traceback
    #         traceback.print_exc()

# Start pet engine thread
pet_engine_thread = Thread(target=pet_engine_cycle, daemon=True)
pet_engine_thread.start()
print("🐾 Pet engine started (runs every 60 seconds)")

# ==================== IMAGE CLEANUP TASK ====================
import time
from threading import Lock

image_cleanup_lock = Lock()

# ❌ DISABLED: File system image cleanup (images stored in database only)
# def cleanup_old_images():
#     """Delete all images except the latest one every 30 seconds"""
#     while True:
#         try:
#             time.sleep(30)
#             with image_cleanup_lock:
#                 uploads_dir = os.path.join(os.getcwd(), 'uploads', 'images')
#                 if not os.path.exists(uploads_dir):
#                     continue
#                 # ... cleanup logic ...
#         except Exception as e:
#             print(f"Error in cleanup task: {e}")

# ❌ DISABLED: Cleanup thread (no file system storage)
# cleanup_thread = Thread(target=cleanup_old_images, daemon=True)
# cleanup_thread.start()
print("✅ Image storage: DATABASE ONLY (no file system cleanup needed)")

# ==================== STEP STATISTICS UPDATE TASK ====================

def update_step_statistics():
    """Periodically aggregate step data and update statistics table"""
    while True:
        try:
            time.sleep(60)  # Update every 60 seconds
            
            with db_lock:
                conn = get_db_connection()
                if not conn:
                    continue
                
                cursor = conn.cursor()
                
                # Get today's date
                today = datetime.now().date()
                device_id = 'ESP32_001'
                
                # Calculate today's step statistics
                cursor.execute('''
                    SELECT 
                        COUNT(*) as batch_count,
                        SUM(step_count) as total_today,
                        MAX(step_count) as peak_steps,
                        AVG(CASE WHEN step_count > 0 THEN step_count ELSE NULL END) as avg_steps_per_batch
                    FROM sensor_readings
                    WHERE device_id = ? AND DATE(timestamp) = ?
                ''', (device_id, today))
                
                result = cursor.fetchone()
                if result:
                    batch_count, total_today, peak_steps, avg_steps = result
                    total_today = total_today or 0
                    peak_steps = peak_steps or 0
                    avg_steps = avg_steps or 0.0
                    
                    # Determine activity level based on total steps
                    if total_today == 0:
                        activity = 'INACTIVE'
                    elif total_today < 500:
                        activity = 'LOW'
                    elif total_today < 2000:
                        activity = 'MODERATE'
                    elif total_today < 5000:
                        activity = 'HIGH'
                    else:
                        activity = 'VERY_HIGH'
                    
                    # Update or insert today's statistics
                    cursor.execute('''
                        UPDATE step_statistics
                        SET total_steps = ?, peak_steps = ?, avg_step_interval = ?, 
                            activity_level = ?, updated_at = CURRENT_TIMESTAMP
                        WHERE device_id = ? AND date_recorded = ?
                    ''', (total_today, peak_steps, avg_steps, activity, device_id, today))
                    
                    if cursor.rowcount == 0:
                        # Insert new record if doesn't exist
                        cursor.execute('''
                            INSERT INTO step_statistics 
                            (device_id, date_recorded, total_steps, peak_steps, avg_step_interval, activity_level)
                            VALUES (?, ?, ?, ?, ?, ?)
                        ''', (device_id, today, total_today, peak_steps, avg_steps, activity))
                    
                    conn.commit()
                    print(f"📊 Step statistics updated: {total_today} total | {peak_steps} peak | Activity: {activity}")
                
                conn.close()
        
        except Exception as e:
            print(f"❌ Error in step statistics update: {e}")

# Start statistics update thread
stats_thread = Thread(target=update_step_statistics, daemon=True)
stats_thread.start()
print("Step statistics update task started (runs every 60 seconds)")

# Connected clients  
connected_clients = set()

# ==================== WebSocket Events ====================

@socketio.on('connect')
def handle_connect():
    try:
        print(f'Client connected: {request.sid}')
        connected_clients.add(request.sid)
        def emit_connection():
            with app.app_context():
                socketio.emit('connection_response', {'status': 'Connected to dashboard'})
        socketio.start_background_task(emit_connection)
    except Exception as e:
        print(f'Connection error: {e}')

@socketio.on('disconnect')
def handle_disconnect():
    try:
        print(f'Client disconnected: {request.sid}')
        connected_clients.discard(request.sid)
    except Exception as e:
        print(f'Disconnect error: {e}')

@socketio.on('sensor_data')
def handle_sensor_data(data):
    """Receive sensor data from ESP32 and broadcast to all connected clients"""
    try:
        print(f'Received sensor data: {data}')
        
        # Store in database
        store_sensor_data(data)
        
        # Broadcast to all connected clients
        def emit_sensor_data():
            with app.app_context():
                socketio.emit('sensor_update', data)
        socketio.start_background_task(emit_sensor_data)
        
        return {'status': 'success', 'message': 'Data received and stored'}
    except Exception as e:
        print(f'Error processing sensor data: {e}')
        return {'status': 'error', 'message': str(e)}

# ==================== REST API Endpoints ====================

@app.route('/')
def index():
    """Serve the main dashboard HTML"""
    return app.send_static_file('index.html')

@app.route('/api/sensor-data', methods=['POST'])
def receive_sensor_data():
    """Receive sensor data from ESP32 and compute orientation on server"""
    try:
        # Validate request
        if not request.is_json:
            return jsonify({'status': 'error', 'message': 'Content-Type must be application/json'}), 400
        
        data = request.get_json()
        if not data:
            return jsonify({'status': 'error', 'message': 'No data received'}), 400
        
        # Extract accelerometer data
        accel_x = data.get('accel_x', 0)
        accel_y = data.get('accel_y', 0)
        accel_z = data.get('accel_z', 0)
        
        total_steps_batch = int(data.get('step_count', 0))
        
        # 🐾 AUTHORITATIVE PET STATE from Hardware
        pet_state_data = data.get('pet_state')
        if pet_state_data:
            print(f"🛰️ Syncing pet state from hardware: {pet_state_data}")
            sync_pet_state_to_db(data.get('device_id', 'ESP32_001'), pet_state_data)

        # 👣 STEP COUNT from ESP32 hardware
        import time
        current_time = time.time()

        total_steps_batch = int(data.get('step_count', 0))
        if total_steps_batch > 0:
            global last_walking_time
            last_walking_time = current_time  # Mark walking active for OLED is_walking flag
            with step_counter_lock:
                step_count_global += total_steps_batch
            print(f'👣 HW Steps: +{total_steps_batch} | Total: {step_count_global}')
        
        # 🧭 COMPUTE ORIENTATION ON SERVER (moved from ESP32)
        direction, confidence = detect_device_orientation(accel_x, accel_y, accel_z)
        
        # Add computed values to data
        data['device_orientation'] = direction
        data['orientation_confidence'] = confidence
        data['calibrated_ax'] = accel_x
        data['calibrated_ay'] = accel_y
        data['calibrated_az'] = accel_z
        data['step_count'] = total_steps_batch
        
        # Reduced logging - only show if steps detected or errors
        if total_steps_batch > 0:
            print(f'👣 Steps: {total_steps_batch} | Total: {step_count_global} | Dir: {direction}')
        
        # Store safely in database (including computed orientation)
        success = store_sensor_data(data)
        if not success:
            return jsonify({'status': 'error', 'message': 'Database storage failed'}), 500
        
        # Broadcast to connected clients with orientation data
        try:
            def emit_sensor_update():
                with app.app_context():
                    socketio.emit('sensor_update', {
                        'timestamp': datetime.now().isoformat(),
                        'device_id': data.get('device_id', 'ESP32_001'),
                        'accel_x': accel_x,
                        'accel_y': accel_y, 
                        'accel_z': accel_z,
                        'gyro_x': data.get('gyro_x', 0),
                        'gyro_y': data.get('gyro_y', 0),
                        'gyro_z': data.get('gyro_z', 0),
                        'mic_level': data.get('mic_level', 0),
                        'sound_data': data.get('sound_data', 0),
                        'chip_temperature': data.get('chip_temperature', 0)
                    })
            
            def emit_orientation():
                with app.app_context():
                    socketio.emit('orientation_update', {
                        'timestamp': datetime.now().isoformat(),
                        'device_id': data.get('device_id', 'ESP32_001'),
                        'direction': direction,
                        'calibrated_ax': accel_x,
                        'calibrated_ay': accel_y,
                        'calibrated_az': accel_z,
                        'confidence': confidence
                    })
            
            socketio.start_background_task(emit_sensor_update)
            socketio.start_background_task(emit_orientation)
            
            # 👟 Broadcast step counter update if steps were detected in this batch
            if total_steps_batch > 0:
                # Reduced logging for performance
                broadcast_step_counter_update(step_count_global, 0)
                
                # 📊 Update step statistics immediately after detection
                update_step_stats_immediate(device_id=data.get('device_id', 'ESP32_001'), steps=total_steps_batch)
        except Exception as e:
            print(f'Warning: SocketIO broadcast failed: {e}')
        
        return jsonify({'status': 'success', 'message': 'Data received and orientation computed'}), 200
    
    except Exception as e:
        print(f'❌ Sensor data error: {e}')
        return jsonify({'status': 'error', 'message': 'Internal server error'}), 500

@app.route('/api/orientation-data', methods=['POST'])
def receive_orientation_data():
    """Receive calibrated orientation/direction data from ESP32"""
    try:
        # Validate request
        if not request.is_json:
            return jsonify({'status': 'error', 'message': 'Content-Type must be application/json'}), 400
        
        data = request.get_json()
        if not data:
            return jsonify({'status': 'error', 'message': 'No data received'}), 400
        
        # Extract orientation data
        direction = data.get('direction', 'UNKNOWN')
        calibrated_ax = data.get('calibrated_ax', 0.0)
        calibrated_ay = data.get('calibrated_ay', 0.0) 
        calibrated_az = data.get('calibrated_az', 0.0)
        confidence = data.get('confidence', 0.0)
        device_id = data.get('device_id', 'ESP32_001')
        
        print(f'🧭 Direction: {direction} | CAL_AX: {calibrated_ax:.3f} CAL_AY: {calibrated_ay:.3f} CAL_AZ: {calibrated_az:.3f} | Conf: {confidence:.1f}%')
        
        # Store orientation data in database
        with db_lock:
            conn = get_db_connection()
            if conn:
                try:
                    cursor = conn.cursor()
                    
                    # Update latest sensor record with orientation data
                    cursor.execute('''
                        UPDATE sensor_readings 
                        SET device_orientation = ?, orientation_confidence = ?, 
                            calibrated_ax = ?, calibrated_ay = ?, calibrated_az = ?
                        WHERE id = (SELECT MAX(id) FROM sensor_readings WHERE device_id = ?)
                    ''', (direction, confidence, calibrated_ax, calibrated_ay, calibrated_az, device_id))
                    
                    if cursor.rowcount == 0:
                        # If no sensor record exists, create one with orientation data only
                        cursor.execute('''
                            INSERT INTO sensor_readings (device_id, device_orientation, orientation_confidence,
                                                        calibrated_ax, calibrated_ay, calibrated_az, timestamp)
                            VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)
                        ''', (device_id, direction, confidence, calibrated_ax, calibrated_ay, calibrated_az))
                    
                    conn.commit()
                    print(f"✅ Stored orientation data for {device_id}")
                
                except sqlite3.Error as e:
                    print(f"❌ Database error: {e}")
                    return jsonify({'status': 'error', 'message': 'Database storage failed'}), 500
                finally:
                    conn.close()
        
        # Broadcast orientation update to connected clients
        try:
            def emit_orientation_update():
                with app.app_context():
                    socketio.emit('orientation_update', {
                        'timestamp': datetime.now().isoformat(),
                        'device_id': device_id,
                        'direction': direction,
                        'calibrated_ax': calibrated_ax,
                        'calibrated_ay': calibrated_ay,
                        'calibrated_az': calibrated_az,
                        'confidence': confidence
                    })
            socketio.start_background_task(emit_orientation_update)
        except Exception as e:
            print(f'Warning: SocketIO orientation broadcast failed: {e}')
        
        return jsonify({'status': 'success', 'message': 'Orientation data received'}), 200
    
    except Exception as e:
        print(f'❌ Orientation data error: {e}')
        return jsonify({'status': 'error', 'message': 'Internal server error'}), 500

# ❌ DISABLED: /api/camera-upload endpoint removed
# Only using /upload (binary) endpoint for image uploads


# ❌ DISABLED: Audio upload endpoint (ESP32 not sending audio)
# The try-except block below was part of the original audio upload handler
# Keeping this section commented out for future reference
# 
# Original code structure:
# @app.route('/upload-audio', methods=['POST'])
# def upload_audio_data():
#     try:
#         ... audio handling logic ...
#         return jsonify({'status': 'success'}), 200
#     except Exception as e:
#         print(f'❌ Error uploading audio: {e}')
#         return jsonify({'status': 'error', 'message': str(e)}), 500

# ✅ Placeholder endpoint for compatibility (returns disabled message)
@app.route('/upload-audio', methods=['POST'])
def upload_audio_data():
    """Audio upload disabled - ESP32 not sending audio data"""
    return jsonify({
        'status': 'disabled',
        'message': 'Audio upload is currently disabled (ESP32 not sending audio)'
    }), 200

@app.route('/upload', methods=['POST'])
def upload_binary_image():
    """
    Receive binary image from ESP32, save it to the database as BLOB, and also save as a file.
    Frontend uses local PC IP address (192.168.1.6) for image URLs.
    Database BLOB storage maintained for persistence.
    
    NEW: Image upload = feeding the pet (the frame IS the food data)
    - Automatically reduces hunger when image is received
    - No AI food detection required
    """
    try:
        image_data = request.get_data()
        if not image_data:
            return 'ERROR', 400
        
        device_id = request.args.get('device_id', 'ESP32_001')
        
        # ✅ DATABASE-ONLY STORAGE (no file system)
        import time
        filename = f"esp32_{int(time.time())}.jpg"
        
        # ❌ DISABLED: File system storage (commented out)
        # uploads_dir = os.path.join(os.getcwd(), 'uploads', 'images')
        # os.makedirs(uploads_dir, exist_ok=True)
        # filepath = os.path.join(uploads_dir, filename)
        # with open(filepath, "wb") as f:
        #     f.write(image_data)
        # print(f"✅ Image saved locally: {filename} ({len(image_data)} bytes)")
        
        # ✅ Save image binary to database ONLY (camera_image BLOB)
        with db_lock:
            conn = get_db_connection()
            if conn:
                cursor = conn.cursor()
                cursor.execute('''
                    INSERT INTO sensor_readings (device_id, camera_image, image_filename, timestamp)
                    VALUES (?, ?, ?, CURRENT_TIMESTAMP)
                ''', (device_id, image_data, filename))
                image_id = cursor.lastrowid
                conn.commit()
                conn.close()
        print(f"✅ Image saved to DATABASE ONLY (id={image_id}, {len(image_data)} bytes)")
        
        # TRIGGER BACKGROUND AI ANALYSIS (BLIP MODEL)
        socketio.start_background_task(analyze_and_store_image_blob, image_id, image_data)
        print(f"🤖 [UPLOAD] Triggered background AI analysis for image #{image_id}")
        
        # NOTE: PET FEEDING logic moved to LOCAL HARDWARE for instant response.
        # Server mirroring happens via /api/sensor-data sync.
        
        # Return database-based URL for frontend
        db_url = f'/api/image/{image_id}'
        
        return jsonify({
            'status': 'success',
            'image_id': image_id,
            'image_url': db_url,
            'filename': filename,
            'pet_fed': True,
            'hunger_reduced': 40
        }), 200
    except Exception as e:
        print(f"❌ Error uploading image: {e}")
        return 'ERROR', 500


@app.route('/api/latest', methods=['GET'])
def get_latest_data():
    """Get latest sensor readings (excluding binary data for JSON compatibility)"""
    try:
        limit = request.args.get('limit', 20, type=int)
        conn = sqlite3.connect(DB_PATH)
        conn.row_factory = sqlite3.Row
        cursor = conn.cursor()
        
        # Check if audio_data column exists
        cursor.execute("PRAGMA table_info(sensor_readings)")
        columns = [column[1] for column in cursor.fetchall()]
        has_audio_column = 'audio_data' in columns
        
        # Select data based on available columns
        if has_audio_column:
            cursor.execute('''
                SELECT id, timestamp, accel_x, accel_y, accel_z, 
                       gyro_x, gyro_y, gyro_z, mic_level, sound_data, image_filename,
                       chip_temperature, device_orientation as direction, 
                       orientation_confidence as confidence,
                       calibrated_ax, calibrated_ay, calibrated_az,
                       CASE WHEN camera_image IS NOT NULL THEN 1 ELSE 0 END as has_image,
                       CASE WHEN audio_data IS NOT NULL THEN 1 ELSE 0 END as has_audio
                FROM sensor_readings 
                ORDER BY id DESC 
                LIMIT ?
            ''', (limit,))
        else:
            cursor.execute('''
                SELECT id, timestamp, accel_x, accel_y, accel_z, 
                       gyro_x, gyro_y, gyro_z, mic_level, sound_data, image_filename,
                       chip_temperature, device_orientation as direction, 
                       orientation_confidence as confidence,
                       calibrated_ax, calibrated_ay, calibrated_az,
                       CASE WHEN camera_image IS NOT NULL THEN 1 ELSE 0 END as has_image,
                       0 as has_audio
                FROM sensor_readings 
                ORDER BY id DESC 
                LIMIT ?
            ''', (limit,))
        
        rows = cursor.fetchall()
        records = []
        
        for row in rows:
            record = dict(row)
            # Convert timestamp to string for JSON compatibility
            if record.get('timestamp'):
                record['timestamp'] = str(record['timestamp'])
            records.append(record)
        
        conn.close()
        
        return jsonify({
            'success': True,
            'records': records,
            'count': len(records)
        }), 200
    except Exception as e:
        print(f'Error fetching data: {e}')
        return jsonify({'success': False, 'error': str(e)}), 400

@app.route('/api/latest-image', methods=['GET'])
def get_latest_image():
    """Get the latest image as base64 from database with AI caption"""
    try:
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        
        cursor.execute('''
            SELECT id, camera_image, image_filename, ai_caption FROM sensor_readings 
            WHERE camera_image IS NOT NULL 
            ORDER BY id DESC 
            LIMIT 1
        ''')
        
        result = cursor.fetchone()
        conn.close()
        
        if result and result[1]:
            image_id = result[0]
            image_binary = result[1]
            image_filename = result[2] if result[2] else f"image_{image_id}.jpg"
            ai_caption = result[3] if result[3] else "Waiting for AI analysis..."
            
            # ✅ Return base64 image data from database
            image_base64 = base64.b64encode(image_binary).decode('utf-8')
            image_url = f'data:image/jpeg;base64,{image_base64}'
            
            return jsonify({
                'success': True,
                'image_url': image_url,
                'image_id': image_id,
                'filename': image_filename,
                'ai_caption': ai_caption,
                'source': 'database'
            }), 200
        else:
            return jsonify({
                'success': False,
                'message': 'No images found in database'
            }), 404
    except Exception as e:
        print(f'Error fetching latest image: {e}')
        return jsonify({'success': False, 'error': str(e)}), 400

@app.route('/api/stats', methods=['GET'])
def get_statistics():
    """Get database statistics"""
    try:
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        
        cursor.execute('SELECT COUNT(*) FROM sensor_readings')
        total_records = cursor.fetchone()[0]
        
        cursor.execute('SELECT AVG(accel_x), AVG(accel_y), AVG(accel_z) FROM sensor_readings')
        accel_avg = cursor.fetchone()
        
        cursor.execute('SELECT MIN(timestamp), MAX(timestamp) FROM sensor_readings')
        date_range = cursor.fetchone()
        
        conn.close()
        
        return jsonify({
            'total_records': total_records,
            'accel_average': {
                'x': accel_avg[0],
                'y': accel_avg[1],
                'z': accel_avg[2]
            },
            'date_range': {
                'start': date_range[0],
                'end': date_range[1]
            }
        }), 200
    except Exception as e:
        print(f'Error fetching statistics: {e}')
        return jsonify({'error': str(e)}), 400

@app.route('/api/export', methods=['GET'])
def export_data():
    """Export sensor data as JSON (excludes binary data)"""
    try:
        conn = sqlite3.connect(DB_PATH)
        conn.row_factory = sqlite3.Row
        cursor = conn.cursor()
        
        # Export only JSON-serializable data
        cursor.execute('''
            SELECT id, timestamp, accel_x, accel_y, accel_z, 
                   gyro_x, gyro_y, gyro_z, mic_level, sound_data,
                   CASE WHEN camera_image IS NOT NULL THEN 1 ELSE 0 END as has_image,
                   CASE WHEN audio_data IS NOT NULL THEN 1 ELSE 0 END as has_audio
            FROM sensor_readings 
            ORDER BY timestamp
        ''')
        
        rows = cursor.fetchall()
        records = []
        
        for row in rows:
            record = dict(row)
            # Convert timestamp to string for JSON compatibility
            if record.get('timestamp'):
                record['timestamp'] = str(record['timestamp'])
            records.append(record)
        
        conn.close()
        
        response = app.response_class(
            response=json.dumps(records, indent=2),
            status=200,
            mimetype='application/json'
        )
        response.headers['Content-Disposition'] = f'attachment;filename=sensor_data_{datetime.now().strftime("%Y%m%d_%H%M%S")}.json'
        return response
    except Exception as e:
        print(f'Error exporting data: {e}')
        return jsonify({'error': str(e)}), 400

@app.route('/api/clear', methods=['POST'])
def clear_database():
    """Clear all data from database"""
    try:
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        cursor.execute('DELETE FROM sensor_readings')
        conn.commit()
        conn.close()
        
        return jsonify({'message': 'Database cleared successfully'}), 200
    except Exception as e:
        print(f'Error clearing database: {e}')
        return jsonify({'error': str(e)}), 400

@app.route('/api/health', methods=['GET'])
def health_check():
    """Health check endpoint"""
    return jsonify({
        'status': 'healthy',
        'timestamp': datetime.now().isoformat(),
        'database': os.path.exists(DB_PATH)
    }), 200

@app.route('/api/events', methods=['GET'])
def get_important_events():
    """Get important events for ESP32 device"""
    try:
        device_id = request.args.get('device_id', 'ESP32_001')
        
        with db_lock:
            conn = sqlite3.connect(DB_PATH)
            cursor = conn.cursor()
            
            # Get unsent important events for this device
            cursor.execute('''
                SELECT id, event_type, message, created_at
                FROM important_events 
                WHERE device_id = ? AND is_sent = 0
                ORDER BY created_at DESC
                LIMIT 10
            ''', (device_id,))
            
            events = cursor.fetchall()
            conn.close()
        
        if events:
            events_list = []
            for event in events:
                event_id, event_type, message, created_at = event
                events_list.append({
                    "id": event_id,
                    "event_type": event_type, 
                    "message": message,
                    "created_at": created_at,
                    "device_id": device_id
                })
            
            return jsonify({
                "status": "success",
                "events": events_list,
                "count": len(events_list),
                "message": f"Found {len(events_list)} important event(s)"
            }), 200
        else:
            return jsonify({
                "status": "success", 
                "events": [],
                "count": 0,
                "message": "No new important events"
            }), 200
            
    except Exception as e:
        print(f'❌ Error getting events: {e}')
        return jsonify({
            "status": "error",
            "message": "Failed to fetch events",
            "error": str(e)
        }), 500

@app.route('/api/device/event/received', methods=['POST'])
def mark_event_received():
    """Mark event as received by ESP32"""
    try:
        data = request.get_json()
        if not data or 'event_id' not in data:
            return jsonify({
                "status": "error",
                "message": "event_id is required"
            }), 400
            
        event_id = data['event_id']
        device_id = data.get('device_id', 'ESP32_001')
        
        with db_lock:
            conn = get_db_connection()
            if not conn:
                return jsonify({
                    "status": "error",
                    "message": "Database connection failed"
                }), 500
            cursor = conn.cursor()
            
            # Mark event as sent/received
            cursor.execute('''
                UPDATE important_events 
                SET is_sent = 1
                WHERE id = ? AND device_id = ?
            ''', (event_id, device_id))
            
            if cursor.rowcount > 0:
                conn.commit()
                print(f'✅ Event {event_id} marked as received by {device_id}')
                result = {
                    "status": "success",
                    "message": f"Event {event_id} marked as received",
                    "event_id": event_id
                }
            else:
                result = {
                    "status": "error", 
                    "message": "Event not found or already processed",
                    "event_id": event_id
                }
            
            conn.close()
            return jsonify(result), 200 if result["status"] == "success" else 404
            
    except Exception as e:
        print(f'❌ Error marking event received: {e}')
        return jsonify({
            "status": "error",
            "message": "Failed to update event status",
            "error": str(e)
        }), 500

# ==================== PET ACTION ENDPOINTS ====================

@app.route('/api/pet/inject', methods=['POST'])
def pet_inject():
    """Give pet injection - restores health when sick"""
    try:
        data = request.get_json() or {}
        device_id = data.get('device_id', 'ESP32_001')
        
        state = get_pet_state(device_id)
        if not state:
            return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
        
        from datetime import datetime, timedelta
        
        if state['health'] >= 80:
            return jsonify({
                'status': 'success',
                'message': 'Pet is already healthy',
                'health': state['health']
            }), 200
        
        # Set action lock
        updates = {'action_lock': 1}
        update_pet_state_atomic(device_id, updates)
        
        # Injection logic — also clears sick_pending and resumes hunger
        updates = {
            'health': min(100, state['health'] + 20),
            'sick_pending': 0,  # Cure sickness — sick icon disappears, hunger resumes
            'current_emotion': 'RECOVER',
            'emotion_expire_at': (datetime.now() + timedelta(seconds=3)).isoformat(),
            'action_lock': 0
        }
        
        result = update_pet_state_atomic(device_id, updates)
        
        if result:
            print(f"💉 Pet injected: health {state['health']} → {result['health']}")
            return jsonify({
                'status': 'success',
                'message': 'Pet injected successfully',
                'health': result['health'],
                'emotion': result['current_emotion']
            }), 200
        else:
            return jsonify({'status': 'error', 'message': 'Failed to inject pet'}), 500
            
    except Exception as e:
        print(f'❌ Error injecting pet: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/pet/clean', methods=['POST'])
def pet_clean():
    """Clean poop - removes poop_present and resets cleanliness"""
    try:
        data = request.get_json() or {}
        device_id = data.get('device_id', 'ESP32_001')
        
        state = get_pet_state(device_id)
        if not state:
            return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
        
        from datetime import datetime
        
        if not state['poop_present']:
            return jsonify({
                'status': 'success',
                'message': 'No poop to clean',
                'poop_present': False,
                'cleanliness': state['cleanliness']
            }), 200
        
        # Cleaning logic
        updates = {
            'poop_present': 0,
            'poop_timestamp': None,
            'cleanliness': 100,
            'last_clean_time': datetime.now().isoformat(),
            'current_emotion': 'HAPPY',
            'emotion_expire_at': (datetime.now() + timedelta(seconds=3)).isoformat()
        }
        
        result = update_pet_state_atomic(device_id, updates)
        
        if result:
            print(f"🧹 Pet cleaned: poop removed, cleanliness → 100")
            return jsonify({
                'status': 'success',
                'message': 'Pet cleaned successfully',
                'poop_present': False,
                'cleanliness': 100
            }), 200
        else:
            return jsonify({'status': 'error', 'message': 'Failed to clean pet'}), 500
            
    except Exception as e:
        print(f'❌ Error cleaning pet: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/pet/play-result', methods=['POST'])
def pet_play_result():
    """Process play game result - affects happiness"""
    try:
        data = request.get_json() or {}
        device_id = data.get('device_id', 'ESP32_001')
        result_type = data.get('result', 'LOSE').upper()  # WIN or LOSE
        
        state = get_pet_state(device_id)
        if not state:
            return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
        
        from datetime import datetime, timedelta
        
        # Set action lock
        updates = {'action_lock': 1}
        update_pet_state_atomic(device_id, updates)
        
        # Play result logic
        if result_type == 'WIN':
            updates = {
                'happiness': min(100, state['happiness'] + 20),
                'last_play_time': datetime.now().isoformat(),
                'current_emotion': 'WIN',
                'emotion_expire_at': (datetime.now() + timedelta(seconds=3)).isoformat(),
                'action_lock': 0
            }
        else:  # LOSE
            updates = {
                'happiness': max(0, state['happiness'] - 10),
                'last_play_time': datetime.now().isoformat(),
                'current_emotion': 'LOSE',
                'emotion_expire_at': (datetime.now() + timedelta(seconds=3)).isoformat(),
                'action_lock': 0
            }
        
        result = update_pet_state_atomic(device_id, updates)
        
        if result:
            print(f"🎮 Play result: {result_type} - happiness {state['happiness']} → {result['happiness']}")
            return jsonify({
                'status': 'success',
                'message': f'Play result: {result_type}',
                'happiness': result['happiness'],
                'emotion': result['current_emotion'],
                'result': result_type
            }), 200
        else:
            return jsonify({'status': 'error', 'message': 'Failed to process play result'}), 500
            
    except Exception as e:
        print(f'❌ Error processing play result: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/pet/cover-happy', methods=['POST'])
def pet_cover_happy():
    """Camera cover interaction detected — happiness +5 (max 100)"""
    try:
        data = request.get_json() or {}
        device_id = data.get('device_id', 'ESP32_001')
        
        state = get_pet_state(device_id)
        if not state:
            return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
        
        new_happiness = min(100, state['happiness'] + 2)
        updates = {'happiness': new_happiness}
        result = update_pet_state_atomic(device_id, updates)
        
        if result:
            print(f"😀 Cover interaction: happiness {state['happiness']} → {result['happiness']}")
            return jsonify({
                'status': 'success',
                'happiness': result['happiness']
            }), 200
        else:
            return jsonify({'status': 'error', 'message': 'Failed to update happiness'}), 500

    except Exception as e:
        print(f'❌ Error in cover-happy: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/game/reward', methods=['POST'])
def game_reward():
    """Process catch-food game reward - store score and KakuCoin"""
    try:
        data = request.get_json() or {}
        device_id = data.get('device_id', 'ESP32_001')
        score = data.get('score', 0)
        kakucoin = data.get('kakucoin', 0)
        play_duration = data.get('play_duration', 0)
        game_type = data.get('game_type', 'CATCH_FOOD')
        
        conn = get_db_connection()
        if not conn:
            return jsonify({'status': 'error', 'message': 'Database connection failed'}), 500
        
        try:
            cursor = conn.cursor()
            cursor.execute('''
                INSERT INTO game_rewards (device_id, game_type, score, kakucoin, play_duration)
                VALUES (?, ?, ?, ?, ?)
            ''', (device_id, game_type, score, kakucoin, play_duration))
            conn.commit()
            
            print(f"🎮 Game reward stored: {game_type} - Score: {score}, KakuCoin: {kakucoin}, Duration: {play_duration}s")
            
            return jsonify({
                'status': 'success',
                'message': 'Game reward stored successfully',
                'score': score,
                'kakucoin': kakucoin,
                'game_type': game_type
            }), 200
            
        except sqlite3.Error as e:
            print(f'❌ Database error storing game reward: {e}')
            return jsonify({'status': 'error', 'message': f'Database error: {str(e)}'}), 500
        finally:
            conn.close()
            
    except Exception as e:
        print(f'❌ Error processing game reward: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/pet/menu', methods=['POST'])
def pet_menu():
    """Switch current menu (MAIN, HEALTH, CLEAN, FEED, PLAY)"""
    try:
        data = request.get_json() or {}
        device_id = data.get('device_id', 'ESP32_001')
        menu = data.get('menu', 'MAIN').upper()
        
        valid_menus = ['MAIN', 'HEALTH', 'CLEAN', 'FEED', 'PLAY']
        if menu not in valid_menus:
            return jsonify({
                'status': 'error',
                'message': f'Invalid menu. Must be one of: {valid_menus}'
            }), 400
        
        state = get_pet_state(device_id)
        if not state:
            return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
        
        updates = {'current_menu': menu}
        result = update_pet_state_atomic(device_id, updates)
        
        if result:
            print(f"📱 Menu changed: {state['current_menu']} → {menu}")
            return jsonify({
                'status': 'success',
                'message': f'Menu changed to {menu}',
                'current_menu': menu
            }), 200
        else:
            return jsonify({'status': 'error', 'message': 'Failed to change menu'}), 500
            
    except Exception as e:
        print(f'❌ Error changing menu: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/pet/state', methods=['GET'])
def get_pet_state_api():
    """Get current pet state (for debugging/dashboard)"""
    try:
        device_id = request.args.get('device_id', 'ESP32_001')
        state = get_pet_state(device_id)
        
        if not state:
            return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
        
        return jsonify({
            'status': 'success',
            'pet_state': state
        }), 200
        
    except Exception as e:
        print(f'❌ Error getting pet state: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

# ==================== Database Functions ====================

def update_step_stats_immediate(device_id, steps):
    """Immediately update step statistics when steps are detected"""
    try:
        with db_lock:
            conn = get_db_connection()
            if not conn:
                return
            
            cursor = conn.cursor()
            today = datetime.now().date()
            
            # Check if today's stats exist
            cursor.execute('''
                SELECT total_steps, peak_steps FROM step_statistics
                WHERE device_id = ? AND date_recorded = ?
            ''', (device_id, today))
            
            existing = cursor.fetchone()
            
            if existing:
                # Update existing stats
                current_total, current_peak = existing
                new_total = (current_total or 0) + steps
                new_peak = max(current_peak or 0, steps)
                
                cursor.execute('''
                    UPDATE step_statistics
                    SET total_steps = ?, peak_steps = ?, updated_at = CURRENT_TIMESTAMP
                    WHERE device_id = ? AND date_recorded = ?
                ''', (new_total, new_peak, device_id, today))
            else:
                # Create new stats record
                cursor.execute('''
                    INSERT INTO step_statistics 
                    (device_id, date_recorded, total_steps, peak_steps, activity_level)
                    VALUES (?, ?, ?, ?, ?)
                ''', (device_id, today, steps, steps, 'LOW'))
            
            conn.commit()
            conn.close()
    
    except Exception as e:
        print(f"❌ Error updating immediate stats: {e}")

def store_sensor_data(data):
    """Store sensor data with thread-safe database access, orientation computation, step counting, and event detection"""
    with db_lock:
        conn = get_db_connection()
        if not conn:
            return False
        
        try:
            cursor = conn.cursor()
            
            # Store sensor data including orientation and steps (now computed on server)
            cursor.execute('''
                INSERT INTO sensor_readings 
                (device_id, accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z, mic_level,
                 device_orientation, orientation_confidence, calibrated_ax, calibrated_ay, calibrated_az, step_count, chip_temperature)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ''', (
                data.get('device_id', 'ESP32_001'),
                data.get('accel_x', 0),
                data.get('accel_y', 0),
                data.get('accel_z', 0),
                data.get('gyro_x', 0),
                data.get('gyro_y', 0),
                data.get('gyro_z', 0),
                data.get('mic_level', 0),
                data.get('device_orientation', 'UNKNOWN'),
                data.get('orientation_confidence', 0),
                data.get('calibrated_ax', 0),
                data.get('calibrated_ay', 0),
                data.get('calibrated_az', 0),
                data.get('step_count', 0),
                data.get('chip_temperature', 0)
            ))
            
            # 🚨 EVENT DETECTION LOGIC
            device_id = data.get('device_id', 'ESP32_001')
            
            # Check for high sound event (mic_level > 80)
            mic_level = data.get('mic_level', 0)
            if mic_level > 80:
                cursor.execute('''
                    INSERT INTO important_events (device_id, event_type, message, is_sent)
                    VALUES (?, ?, ?, ?)
                ''', (device_id, 'high_sound', f'High sound detected: {mic_level:.1f} dB', 0))
                print(f'🚨 HIGH SOUND EVENT: {mic_level:.1f} dB from {device_id}')
            
            # Check for sudden motion (high acceleration change)
            accel_x = data.get('accel_x', 0)
            accel_y = data.get('accel_y', 0) 
            accel_z = data.get('accel_z', 0)
            
            if accel_x and accel_y and accel_z:
                total_accel = (accel_x**2 + accel_y**2 + accel_z**2)**0.5
                
                # Get previous acceleration for comparison
                cursor.execute('''
                    SELECT accel_x, accel_y, accel_z FROM sensor_readings 
                    WHERE device_id = ? AND accel_x IS NOT NULL 
                    ORDER BY id DESC LIMIT 1 OFFSET 1
                ''', (device_id,))
                prev_reading = cursor.fetchone()
                
                if prev_reading:
                    prev_x, prev_y, prev_z = prev_reading
                    prev_total = (prev_x**2 + prev_y**2 + prev_z**2)**0.5
                    accel_change = abs(total_accel - prev_total)
                    
                    # Sudden motion detected (change > 5 m/s²)
                    if accel_change > 5.0:
                        cursor.execute('''
                            INSERT INTO important_events (device_id, event_type, message, is_sent)
                            VALUES (?, ?, ?, ?)
                        ''', (device_id, 'sudden_motion', f'Sudden motion detected: {accel_change:.2f} m/s² change', 0))
                        print(f'🚨 MOTION EVENT: {accel_change:.2f} m/s² change from {device_id}')
            
            conn.commit()
            return True
            
        except sqlite3.Error as e:
            print(f'❌ Database error storing sensor data: {e}')
            return False
        except Exception as e:
            print(f'❌ Error storing sensor data: {e}')
            return False
        finally:
            conn.close()

# ==================== OLED DISPLAY ANIMATION CONTROL ====================

def _should_show_play_icon(pet: dict) -> bool:
    """Returns True if the play icon should blink on the ESP32 main screen.
    
    Rules:
      - At least 1 hour has passed since last_feed_time
      - Pet is NOT sick (sick_pending == 0)
      - Pet does NOT currently have poop present
    """
    try:
        if pet.get('sick_pending', False):
            return False
        if pet.get('poop_present', False):
            return False
        last_feed = pet.get('last_feed_time')
        if not last_feed:
            return False
        if isinstance(last_feed, str):
            last_feed = datetime.fromisoformat(last_feed)
        elapsed_seconds = (datetime.now() - last_feed).total_seconds()
        return elapsed_seconds > 3600  # 1 hour
    except Exception:
        return False


@app.route('/api/oled-display/get', methods=['GET'])
def get_oled_display():
    """ESP32 polls this endpoint to get what animation to display on OLED
    
    AUTOMATIC MODE ONLY - Displays based on AI Tamagotchi pet_state
    No manual control - pet evolves based on gameplay (health, hunger, care)
    """
    try:
        device_id = request.args.get('device_id', 'ESP32_001')
        
        # Get pet state (automatically determines animation)
        pet = get_pet_state(device_id)
        
        if not pet:
            # Fallback if no pet state exists
            return jsonify({
                'status': 'success',
                'animation_id': 0,
                'stage': 'INFANT',
                'emotion': 'IDLE',
                'current_emotion': 'IDLE',
                'current_menu': 'MAIN',
                'health': 100,
                'hunger': 0,
                'thirst': 0,
                'level': 1,
                'xp': 0,
                'happiness': 100,
                'energy': 100,
                'has_poop': False,
                'is_sick': False,
                'age': 0,
                'mode': 'INITIALIZING',
                'screen_type': 'MAIN',
                'is_walking': False,
                'message': 'No pet state found, using default'
            }), 200
        
        # Backward compatibility for dashboard and legacy ESP32 code
        stage_to_id = {
            'INFANT': 0, 'CHILD': 1, 'ADULT': 2, 'OLD': 3, 'END': 3
        }
        
        response_data = {
            'status': 'success',
            'animation_id': stage_to_id.get(pet.get('stage', 'INFANT'), 0),
            'stage': pet.get('stage', 'INFANT'),
            'emotion': pet.get('current_emotion', 'IDLE'),
            'current_emotion': pet.get('current_emotion', 'IDLE'),
            'health': pet['health'],
            'hunger': pet['hunger'],
            'thirst': pet.get('thirst', 0),
            'level': pet.get('level', 1),
            'xp': pet.get('xp', 0),
            'happiness': pet['happiness'],
            'energy': pet['energy'],
            'has_poop': pet.get('has_poop', False),
            'is_sick': pet.get('is_sick', False),
            'age': pet['age'],
            'mode': 'HARDWARE (Authoritative)',
            'current_menu': pet.get('current_menu', 'MAIN'),
            'timestamp': datetime.now().isoformat()
        }
        
        # Determine specific animation overrides (if any)
        animation_id = response_data['animation_id']
        current_menu = response_data['current_menu']
        
        return jsonify(response_data), 200
        
    except Exception as e:
        print(f'❌ Error getting OLED display: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500



@app.route('/api/oled-display/set', methods=['POST'])
def set_oled_display():
    """Web UI sends POST request to set OLED display animation
    
    Updates oled_display_state table in database.
    Generic endpoint that can accept any animation type for future extensibility.
    """
    try:
        if not request.is_json:
            return jsonify({'status': 'error', 'message': 'Content-Type must be application/json'}), 400
        
        data = request.get_json()
        animation_id = data.get('animation_id')
        animation_type = data.get('animation_type', 'pet')  # Default to pet type
        device_id = data.get('device_id', 'ESP32_001')
        
        # Validate animation_id value
        if animation_id not in [0, 1, 2, 3]:
            return jsonify({'status': 'error', 'message': 'Invalid animation_id. Must be 0-3'}), 400
        
        animation_map = {
            0: "INFANT",
            1: "CHILD", 
            2: "ADULT",
            3: "OLD"
        }
        
        animation_name = animation_map[animation_id]
        
        # Update database with new state
        with db_lock:
            conn = get_db_connection()
            if not conn:
                return jsonify({'status': 'error', 'message': 'Database connection failed'}), 500
            
            try:
                cursor = conn.cursor()
                
                # Update or insert OLED state
                cursor.execute('''
                    UPDATE oled_display_state
                    SET animation_type = ?, animation_id = ?, animation_name = ?, 
                        updated_at = CURRENT_TIMESTAMP, updated_by = 'web_ui'
                    WHERE device_id = ?
                ''', (animation_type, animation_id, animation_name, device_id))
                
                if cursor.rowcount == 0:
                    # Insert if not exists
                    cursor.execute('''
                        INSERT INTO oled_display_state
                        (device_id, animation_type, animation_id, animation_name, updated_by)
                        VALUES (?, ?, ?, ?, ?)
                    ''', (device_id, animation_type, animation_id, animation_name, 'web_ui'))
                
                conn.commit()
                print(f'✅ OLED state updated in database: {animation_id} ({animation_name})')
                print(f'   Device: {device_id} | Type: {animation_type}')
            except sqlite3.Error as e:
                print(f'❌ Database error: {e}')
                return jsonify({'status': 'error', 'message': 'Database update failed'}), 500
            finally:
                conn.close()
        
        # Broadcast animation change to all connected web clients (real-time)
        def emit_oled_change():
            with app.app_context():
                socketio.emit('oled_display_changed', {
                    'animation_type': animation_type,
                    'animation_id': animation_id,
                    'animation_name': animation_name,
                    'device_id': device_id,
                    'timestamp': datetime.now().isoformat()
                })
        
        socketio.start_background_task(emit_oled_change)
        
        return jsonify({
            'status': 'success',
            'animation_type': animation_type,
            'animation_id': animation_id,
            'animation_name': animation_name,
            'device_id': device_id,
            'message': f'OLED display set to: {animation_name}'
        }), 200
        
    except Exception as e:
        print(f'❌ Error setting OLED display: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/api/oled-display/reset', methods=['POST'])
def reset_oled_display():
    """Reset OLED display to AI automatic mode
    
    Clears manual button selection, allowing AI pet state to control the display
    """
    try:
        data = request.get_json() if request.is_json else {}
        device_id = data.get('device_id', 'ESP32_001')
        
        # Delete manual selection from database
        with db_lock:
            conn = get_db_connection()
            if not conn:
                return jsonify({'status': 'error', 'message': 'Database connection failed'}), 500
            
            try:
                cursor = conn.cursor()
                cursor.execute('DELETE FROM oled_display_state WHERE device_id = ?', (device_id,))
                conn.commit()
                print(f'✅ OLED display reset to AI mode for device: {device_id}')
            except sqlite3.Error as e:
                print(f'❌ Database error: {e}')
                return jsonify({'status': 'error', 'message': 'Database reset failed'}), 500
            finally:
                conn.close()
        
        # Broadcast reset to all connected web clients
        def emit_oled_reset():
            with app.app_context():
                socketio.emit('oled_display_reset', {
                    'device_id': device_id,
                    'mode': 'AI',
                    'timestamp': datetime.now().isoformat()
                })
        
        socketio.start_background_task(emit_oled_reset)
        
        return jsonify({
            'status': 'success',
            'mode': 'AI',
            'device_id': device_id,
            'message': 'OLED display reset to AI automatic mode'
        }), 200
        
    except Exception as e:
        print(f'❌ Error resetting OLED display: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/device/startup-complete', methods=['POST'])
def device_startup_complete():
    """Handle ESP32 startup notification
    
    Called by ESP32 after infant animation completes.
    RESETS pet to INFANT stage in database (fresh start on every boot).
    Returns initial display configuration.
    """
    try:
        data = request.get_json() if request.is_json else {}
        device_id = data.get('device_id', 'ESP32_001')
        status = data.get('status', 'unknown')
        pet_stage = data.get('pet_stage', 0)
        
        print(f'✅ Device startup notification received from {device_id}')
        print(f'   Status: {status} | Pet Stage: {pet_stage}')

        # ---- OTA completion detection ----
        # If ESP32 rebooted after OTA, mark it as done regardless of what state it was stuck in
        ota_state = ota_progress_state.get(device_id, {})
        if ota_state.get('ota_active') or ota_state.get('ota_status') in ('waiting', 'checking', 'downloading', 'flashing', 'rebooting'):
            ota_progress_state[device_id] = {
                **ota_state,
                'ota_active': False,
                'ota_status': 'done',
                'progress': 100,
                'message': f'✅ Flashed successfully! Device is back online.',
            }
            print(f'🎉 OTA complete for {device_id} — device rebooted with v{ota_state.get("target_version", "?")}')
        
        # RESET TO INFANT on every device startup
        animation_id = 0  # INFANT
        animation_name = 'INFANT'
        show_home_icon = False
        screen_type = 'MAIN'
        
        # UPDATE database to INFANT stage
        with db_lock:
            conn = get_db_connection()
            if conn:
                try:
                    cursor = conn.cursor()
                    
                    # Reset OLED display state to INFANT
                    cursor.execute('''
                        UPDATE oled_display_state
                        SET animation_id = ?, 
                            animation_name = ?, 
                            animation_type = 'pet',
                            show_home_icon = ?,
                            show_food_icon = 0,
                            show_poop_icon = 0,
                            screen_type = ?,
                            updated_at = CURRENT_TIMESTAMP,
                            updated_by = 'device_startup'
                        WHERE device_id = ?
                    ''', (animation_id, animation_name, show_home_icon, screen_type, device_id))
                    
                    if cursor.rowcount == 0:
                        # Insert if not exists
                        cursor.execute('''
                            INSERT INTO oled_display_state
                            (device_id, animation_id, animation_name, animation_type, show_home_icon, screen_type, updated_by)
                            VALUES (?, ?, ?, 'pet', ?, ?, 'device_startup')
                        ''', (device_id, animation_id, animation_name, show_home_icon, screen_type))
                    
                    # Reset pet_state to INFANT with fresh stats
                    cursor.execute('''
                        UPDATE pet_state
                        SET age = 0,
                            stage = 'INFANT',
                            health = 100,
                            hunger = 0,
                            cleanliness = 100,
                            happiness = 100,
                            energy = 100,
                            poop_present = 0,
                            poop_timestamp = NULL,
                            digestion_due_time = NULL,
                            current_menu = 'MAIN',
                            current_emotion = 'IDLE',
                            emotion_expire_at = NULL,
                            action_lock = 0,
                            last_feed_time = NULL,
                            last_play_time = NULL,
                            last_sleep_time = NULL,
                            last_clean_time = NULL,
                            last_age_increment = CURRENT_TIMESTAMP,
                            updated_at = CURRENT_TIMESTAMP
                        WHERE device_id = ?
                    ''', (device_id,))
                    
                    if cursor.rowcount == 0:
                        # Insert if pet_state doesn't exist
                        cursor.execute('''
                            INSERT INTO pet_state 
                            (device_id, age, stage, health, hunger, cleanliness, happiness, energy,
                             current_menu, current_emotion, last_age_increment)
                            VALUES (?, 0, 'INFANT', 100, 0, 100, 100, 100, 'MAIN', 'IDLE', CURRENT_TIMESTAMP)
                        ''', (device_id,))
                    
                    conn.commit()
                    print(f'🔄 Database RESET to INFANT for {device_id} (display + pet state)')
                    
                finally:
                    conn.close()
        
        return jsonify({
            'status': 'success',
            'animation_id': animation_id,
            'animation_name': animation_name,
            'show_home_icon': show_home_icon,
            'screen_type': screen_type,
            'current_menu': 'MAIN',
            'message': f'Pet reset to INFANT - initial display state sent to {device_id}'
        }), 200
        
    except Exception as e:
        print(f'❌ Error handling device startup: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/oled-display/home-icon-toggle', methods=['POST'])
def toggle_home_icon():
    """Toggle home icon display on OLED
    
    Updates show_home_icon flag in database and returns new state
    """
    try:
        data = request.get_json() if request.is_json else {}
        device_id = data.get('device_id', 'ESP32_001')
        show_home_icon = data.get('show_home_icon', False)
        
        with db_lock:
            conn = get_db_connection()
            if not conn:
                return jsonify({'status': 'error', 'message': 'Database connection failed'}), 500
            
            try:
                cursor = conn.cursor()
                
                # Update home icon state
                cursor.execute('''
                    UPDATE oled_display_state
                    SET show_home_icon = ?, updated_at = CURRENT_TIMESTAMP, updated_by = 'web_ui'
                    WHERE device_id = ?
                ''', (show_home_icon, device_id))
                
                if cursor.rowcount == 0:
                    # Insert if not exists
                    cursor.execute('''
                        INSERT INTO oled_display_state
                        (device_id, show_home_icon, updated_by)
                        VALUES (?, ?, ?)
                    ''', (device_id, show_home_icon, 'web_ui'))
                
                conn.commit()
                print(f'🏠 Home icon toggled to: {show_home_icon} for device {device_id}')
                
            except sqlite3.Error as e:
                print(f'❌ Database error: {e}')
                return jsonify({'status': 'error', 'message': 'Database update failed'}), 500
            finally:
                conn.close()
        
        # Broadcast change to all web clients
        def emit_home_icon_change():
            with app.app_context():
                socketio.emit('home_icon_changed', {
                    'show_home_icon': show_home_icon,
                    'device_id': device_id,
                    'timestamp': datetime.now().isoformat()
                })
        
        socketio.start_background_task(emit_home_icon_change)
        
        return jsonify({
            'status': 'success',
            'show_home_icon': show_home_icon,
            'device_id': device_id,
            'message': f'Home icon {("enabled" if show_home_icon else "disabled")}'
        }), 200
        
    except Exception as e:
        print(f'❌ Error toggling home icon: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/oled-display/food-icon-toggle', methods=['POST'])
def toggle_food_icon():
    """Toggle food icon display on OLED (indicates pet is hungry)
    
    Updates show_food_icon flag in database and returns new state
    """
    try:
        data = request.get_json() if request.is_json else {}
        device_id = data.get('device_id', 'ESP32_001')
        show_food_icon = data.get('show_food_icon', False)
        
        with db_lock:
            conn = get_db_connection()
            if not conn:
                return jsonify({'status': 'error', 'message': 'Database connection failed'}), 500
            
            try:
                cursor = conn.cursor()
                
                # Update food icon state
                cursor.execute('''
                    UPDATE oled_display_state
                    SET show_food_icon = ?, updated_at = CURRENT_TIMESTAMP, updated_by = 'web_ui'
                    WHERE device_id = ?
                ''', (show_food_icon, device_id))
                
                if cursor.rowcount == 0:
                    # Insert if not exists
                    cursor.execute('''
                        INSERT INTO oled_display_state
                        (device_id, show_food_icon, updated_by)
                        VALUES (?, ?, ?)
                    ''', (device_id, show_food_icon, 'web_ui'))
                
                conn.commit()
                print(f'🍽️  Food icon toggled to: {show_food_icon} for device {device_id}')
                
            except sqlite3.Error as e:
                print(f'❌ Database error: {e}')
                return jsonify({'status': 'error', 'message': 'Database update failed'}), 500
            finally:
                conn.close()
        
        # Broadcast change to all web clients
        def emit_food_icon_change():
            with app.app_context():
                socketio.emit('food_icon_changed', {
                    'show_food_icon': show_food_icon,
                    'device_id': device_id,
                    'timestamp': datetime.now().isoformat()
                })
        
        socketio.start_background_task(emit_food_icon_change)
        
        return jsonify({
            'status': 'success',
            'show_food_icon': show_food_icon,
            'device_id': device_id,
            'message': f'Food icon {("enabled" if show_food_icon else "disabled")}'
        }), 200
        
    except Exception as e:
        print(f'❌ Error toggling food icon: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/api/oled-display/poop-icon-toggle', methods=['POST'])
def toggle_poop_icon():
    """Toggle poop icon display on OLED (indicates pet needs cleaning)
    
    Updates show_poop_icon flag in database and returns new state
    """
    try:
        data = request.get_json() if request.is_json else {}
        device_id = data.get('device_id', 'ESP32_001')
        show_poop_icon = data.get('show_poop_icon', False)
        
        with db_lock:
            conn = get_db_connection()
            if not conn:
                return jsonify({'status': 'error', 'message': 'Database connection failed'}), 500
            
            try:
                cursor = conn.cursor()
                
                # Update poop icon state
                cursor.execute('''
                    UPDATE oled_display_state
                    SET show_poop_icon = ?, updated_at = CURRENT_TIMESTAMP, updated_by = 'web_ui'
                    WHERE device_id = ?
                ''', (show_poop_icon, device_id))
                
                if cursor.rowcount == 0:
                    # Insert if not exists
                    cursor.execute('''
                        INSERT INTO oled_display_state
                        (device_id, show_poop_icon, updated_by)
                        VALUES (?, ?, ?)
                    ''', (device_id, show_poop_icon, 'web_ui'))
                
                conn.commit()
                print(f'💩 Poop icon toggled to: {show_poop_icon} for device {device_id}')
                
            except sqlite3.Error as e:
                print(f'❌ Database error: {e}')
                return jsonify({'status': 'error', 'message': 'Database update failed'}), 500
            finally:
                conn.close()
        
        # Broadcast change to all web clients
        def emit_poop_icon_change():
            with app.app_context():
                socketio.emit('poop_icon_changed', {
                    'show_poop_icon': show_poop_icon,
                    'device_id': device_id,
                    'timestamp': datetime.now().isoformat()
                })
        
        socketio.start_background_task(emit_poop_icon_change)
        
        return jsonify({
            'status': 'success',
            'show_poop_icon': show_poop_icon,
            'device_id': device_id,
            'message': f'Poop icon {("enabled" if show_poop_icon else "disabled")}'
        }), 200
        
    except Exception as e:
        print(f'❌ Error toggling poop icon: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/api/oled-display/menu-switch', methods=['POST'])
def switch_menu():
    """Switch current menu (MAIN/FOOD_MENU/TOILET_MENU)
    
    User-controlled menu switching via frontend or camera cover detection
    """
    try:
        data = request.get_json()
        device_id = data.get('device_id', 'ESP32_001')
        menu = data.get('menu', 'MAIN')  # MAIN, FOOD_MENU, TOILET_MENU
        
        # Validate menu value
        valid_menus = ['MAIN', 'FOOD_MENU', 'TOILET_MENU']
        if menu not in valid_menus:
            return jsonify({'status': 'error', 'message': f'Invalid menu. Must be one of: {valid_menus}'}), 400
        
        # Update pet state with new menu
        with db_lock:
            conn = get_db_connection()
            if conn:
                try:
                    cursor = conn.cursor()
                    cursor.execute('UPDATE pet_state SET current_menu = ? WHERE device_id = ?', (menu, device_id))
                    conn.commit()
                    print(f'📱 Menu switched to: {menu}')
                except Exception as e:
                    print(f'Error switching menu: {e}')
                    return jsonify({'status': 'error', 'message': str(e)}), 500
                finally:
                    conn.close()
        
        # Broadcast menu change to connected clients
        def emit_menu_change():
            socketio.emit('menu_changed', {
                'device_id': device_id,
                'menu': menu
            }, namespace='/')
        
        socketio.start_background_task(emit_menu_change)
        
        return jsonify({
            'status': 'success',
            'current_menu': menu,
            'device_id': device_id,
            'message': f'Menu switched to {menu}'
        }), 200
        
    except Exception as e:
        print(f'❌ Error switching menu: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

# ================= STEP COUNTER ENDPOINTS =================

@app.route('/api/step-counter/get', methods=['GET'])
def get_step_counter():
    """Get current step counter from server
    
    Returns total steps detected by server-side accelerometer analysis
    """
    try:
        device_id = request.args.get('device_id', 'ESP32_001')
        
        # Get total steps from global counter
        total_steps = step_count_global
        
        # Optional: Get daily steps from database
        with db_lock:
            conn = get_db_connection()
            if conn:
                cursor = conn.cursor()
                cursor.execute('''
                    SELECT SUM(step_count) as daily_steps
                    FROM sensor_readings
                    WHERE device_id = ? AND DATE(timestamp) = DATE('now')
                ''', (device_id,))
                result = cursor.fetchone()
                daily_steps = result[0] if result and result[0] else 0
                conn.close()
            else:
                daily_steps = 0
        
        return jsonify({
            'status': 'success',
            'device_id': device_id,
            'total_steps': total_steps,
            'daily_steps': daily_steps or 0,
            'timestamp': datetime.now().isoformat()
        }), 200
        
    except Exception as e:
        print(f'❌ Error getting step counter: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/step-counter/reset', methods=['POST'])
def reset_step_counter():
    """Reset step counter
    
    Resets the global step counter to 0 (fresh session)
    """
    try:
        global step_count_global
        device_id = request.args.get('device_id', 'ESP32_001')
        old_count = step_count_global
        
        # Reset counter
        step_count_global = 0
        
        # Clear the acceleration history for clean slate
        accel_history.clear()
        
        print(f'🔄 Step counter reset: {old_count} → 0')
        
        # Broadcast reset to all clients
        broadcast_step_counter_update(0, 0)
        
        return jsonify({
            'status': 'success',
            'device_id': device_id,
            'reset_from': old_count,
            'new_count': 0,
            'message': 'Step counter reset to 0',
            'timestamp': datetime.now().isoformat()
        }), 200
        
    except Exception as e:
        print(f'❌ Error resetting step counter: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/step-counter/stats', methods=['GET'])
def get_step_stats():
    """Get detailed step counter statistics with daily aggregation and trends
    
    Returns:
    - Daily statistics (total, peak, activity level)
    - Recent batch-level details
    - Comparison with previous data
    - Activity trends
    """
    try:
        device_id = request.args.get('device_id', 'ESP32_001')
        days = request.args.get('days', 7, type=int)  # Last N days
        
        with db_lock:
            conn = get_db_connection()
            if not conn:
                return jsonify({'status': 'error', 'message': 'Database connection failed'}), 500
            
            try:
                cursor = conn.cursor()
                
                # Get daily aggregated statistics
                cursor.execute('''
                    SELECT 
                        date_recorded,
                        total_steps,
                        peak_steps,
                        avg_step_interval,
                        activity_level,
                        updated_at
                    FROM step_statistics
                    WHERE device_id = ? AND date_recorded >= DATE('now', '-' || ? || ' days')
                    ORDER BY date_recorded DESC
                ''', (device_id, days))
                
                daily_stats = [{
                    'date': str(row[0]),
                    'total_steps': row[1],
                    'peak_steps': row[2],
                    'avg_step_interval': round(row[3], 2),
                    'activity_level': row[4],
                    'updated_at': str(row[5])
                } for row in cursor.fetchall()]
                
                # Get today's detailed batch data
                today = datetime.now().date()
                cursor.execute('''
                    SELECT 
                        timestamp,
                        step_count,
                        accel_x, accel_y, accel_z,
                        SUM(step_count) OVER (ORDER BY timestamp) as cumulative_steps
                    FROM sensor_readings
                    WHERE device_id = ? AND DATE(timestamp) = ?
                    ORDER BY id DESC
                    LIMIT 20
                ''', (device_id, today))
                
                batch_details = [{
                    'timestamp': str(row[0]),
                    'steps_in_batch': row[1],
                    'accel': [round(row[2], 3), round(row[3], 3), round(row[4], 3)],
                    'cumulative': row[5]
                } for row in cursor.fetchall()]
                
                # Calculate trends
                cursor.execute('''
                    SELECT 
                        total_steps,
                        activity_level
                    FROM step_statistics
                    WHERE device_id = ? AND date_recorded >= DATE('now', '-7 days')
                    ORDER BY date_recorded ASC
                ''', (device_id,))
                
                weekly_data = cursor.fetchall()
                trend = None
                if len(weekly_data) >= 2:
                    last_week = sum([row[0] or 0 for row in weekly_data])
                    
                    # Compare with previous week
                    cursor.execute('''
                        SELECT SUM(total_steps)
                        FROM step_statistics
                        WHERE device_id = ? 
                        AND date_recorded >= DATE('now', '-14 days')
                        AND date_recorded < DATE('now', '-7 days')
                    ''', (device_id,))
                    
                    prev_week_result = cursor.fetchone()
                    prev_week = prev_week_result[0] or 0
                    
                    if prev_week > 0:
                        trend_percent = ((last_week - prev_week) / prev_week) * 100
                        trend = {
                            'last_week': last_week,
                            'previous_week': prev_week,
                            'change_percent': round(trend_percent, 1),
                            'direction': 'up' if trend_percent > 0 else 'down' if trend_percent < 0 else 'stable'
                        }
                
                return jsonify({
                    'status': 'success',
                    'device_id': device_id,
                    'current_total': step_count_global,
                    'today': str(today),
                    'daily_statistics': daily_stats,
                    'today_details': batch_details,
                    'trend': trend,
                    'summary': {
                        'total_days_tracked': len(daily_stats),
                        'avg_daily_steps': round(sum([s['total_steps'] for s in daily_stats]) / len(daily_stats), 1) if daily_stats else 0,
                        'max_daily_steps': max([s['total_steps'] for s in daily_stats]) if daily_stats else 0,
                        'total_batches_today': len(batch_details)
                    },
                    'timestamp': datetime.now().isoformat()
                }), 200
                
            except sqlite3.Error as e:
                print(f'❌ Database error: {e}')
                return jsonify({'status': 'error', 'message': 'Database query failed'}), 500
            finally:
                conn.close()
    
    except Exception as e:
        print(f'❌ Error getting step stats: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

# ==================== OTA FIRMWARE UPDATE ENDPOINTS ====================

@app.route('/api/firmware/enable-ota', methods=['POST'])
def enable_ota():
    """Dashboard sends POST to enable/disable OTA for a device.
    ESP32 will see ota_update=true in next OLED poll and begin update."""
    try:
        data = request.get_json()
        device_id = data.get('device_id', 'ESP32_001')
        enable = data.get('enable', True)
        
        ota_enabled_devices[device_id] = bool(enable)
        status_str = 'enabled' if enable else 'disabled'
        print(f'🔄 OTA {status_str} for device {device_id}')

        # Track live OTA progress state
        if bool(enable):
            ota_progress_state[device_id] = {
                'ota_status': 'waiting',
                'ota_active': True,
                'progress': 0,
                'target_version': '',
                'message': 'OTA enabled — waiting for device to check in...',
                'timestamp': time.time()
            }
            print(f'📡 OTA progress tracking started for {device_id}')
        else:
            # Clear progress state on disable (unless it is in done/failed terminal state)
            existing = ota_progress_state.get(device_id, {})
            if existing.get('ota_status') not in ('done', 'failed'):
                ota_progress_state.pop(device_id, None)
        
        return jsonify({
            'status': 'success',
            'device_id': device_id,
            'ota_enabled': bool(enable),
            'message': f'OTA {status_str} for {device_id}'
        }), 200
    except Exception as e:
        print(f'❌ Error enabling OTA: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/api/firmware/upload', methods=['POST'])
def upload_firmware():
    """Upload a new firmware .bin file from the dashboard.
    Stores file in firmware/ directory and records metadata in DB."""
    try:
        if 'firmware' not in request.files:
            return jsonify({'status': 'error', 'message': 'No firmware file in request'}), 400
        
        fw_file = request.files['firmware']
        version = request.form.get('version', '').strip()
        notes = request.form.get('notes', '').strip()
        
        if not version:
            return jsonify({'status': 'error', 'message': 'Version is required'}), 400
        
        if not fw_file.filename.endswith('.bin'):
            return jsonify({'status': 'error', 'message': 'Only .bin files are allowed'}), 400
        
        # Read file data and compute checksum
        fw_data = fw_file.read()
        checksum = hashlib.md5(fw_data).hexdigest()
        file_size = len(fw_data)
        
        # Save file as firmware/<version>.bin
        safe_filename = f'firmware_v{version}.bin'
        filepath = os.path.join(FIRMWARE_DIR, safe_filename)
        with open(filepath, 'wb') as f:
            f.write(fw_data)
        
        # Record in database
        conn = get_db_connection()
        if conn:
            try:
                cursor = conn.cursor()
                cursor.execute('''
                    INSERT INTO firmware_versions (version, filename, file_size, checksum, notes)
                    VALUES (?, ?, ?, ?, ?)
                ''', (version, safe_filename, file_size, checksum, notes))
                conn.commit()
            finally:
                conn.close()
        
        print(f'✅ Firmware v{version} uploaded: {safe_filename} ({file_size} bytes, md5:{checksum})')
        
        return jsonify({
            'status': 'success',
            'version': version,
            'filename': safe_filename,
            'file_size': file_size,
            'checksum': checksum,
            'message': f'Firmware v{version} uploaded successfully'
        }), 200
        
    except Exception as e:
        print(f'❌ Error uploading firmware: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/api/firmware/latest', methods=['GET'])
def get_latest_firmware():
    """ESP32 calls this to check if a newer firmware is available.
    Returns version, download URL, and checksum for integrity verification.
    Requires auth token in header for security."""
    try:
        # Verify device auth token
        auth_token = request.headers.get('X-OTA-Token', '')
        if auth_token != OTA_AUTH_TOKEN:
            return jsonify({'status': 'error', 'message': 'Unauthorized'}), 401
        
        device_id = request.args.get('device_id', 'ESP32_001')
        current_version = request.args.get('current_version', '0.0.0')
        
        conn = get_db_connection()
        if not conn:
            return jsonify({'status': 'error', 'message': 'Database unavailable'}), 500
        
        try:
            cursor = conn.cursor()
            cursor.execute('''
                SELECT version, filename, file_size, checksum, notes
                FROM firmware_versions
                ORDER BY id DESC LIMIT 1
            ''')
            row = cursor.fetchone()
            
            if not row:
                return jsonify({
                    'status': 'success',
                    'update_available': False,
                    'message': 'No firmware versions available'
                }), 200
            
            latest_version = row[0]
            filename = row[1]
            file_size = row[2]
            checksum = row[3]
            notes = row[4]
            
            # Simple version comparison (assumes semantic versioning)
            update_available = latest_version != current_version
            
            # Build download URL
            host = request.host_url.rstrip('/')
            download_url = f'{host}/api/firmware/download/{filename}'
            
            # Auto-disable OTA flag after ESP32 checks in
            if device_id in ota_enabled_devices:
                ota_enabled_devices[device_id] = False
            
            return jsonify({
                'status': 'success',
                'update_available': update_available,
                'version': latest_version,
                'current_version': current_version,
                'download_url': download_url,
                'file_size': file_size,
                'checksum': checksum,
                'notes': notes
            }), 200
            
        finally:
            conn.close()
            
    except Exception as e:
        print(f'❌ Error checking firmware: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/api/firmware/download/<filename>', methods=['GET'])
def download_firmware(filename):
    """Serve the firmware .bin file for ESP32 OTA download.
    Uses streaming response to avoid Cloud Run buffering issues.
    Requires auth token for security."""
    try:
        auth_token = request.headers.get('X-OTA-Token', '')
        if auth_token != OTA_AUTH_TOKEN:
            return jsonify({'status': 'error', 'message': 'Unauthorized'}), 401
        
        filepath = os.path.join(FIRMWARE_DIR, filename)
        if not os.path.exists(filepath):
            return jsonify({'status': 'error', 'message': 'Firmware file not found'}), 404
        
        file_size = os.path.getsize(filepath)
        print(f'📦 Serving firmware: {filename} ({file_size} bytes)')

        def generate():
            with open(filepath, 'rb') as f:
                while True:
                    chunk = f.read(8192)
                    if not chunk:
                        break
                    yield chunk

        from flask import Response
        return Response(
            generate(),
            status=200,
            headers={
                'Content-Type': 'application/octet-stream',
                'Content-Length': str(file_size),
                'Content-Disposition': f'attachment; filename={filename}',
                'Cache-Control': 'no-cache, no-store',
                'X-Accel-Buffering': 'no',
                'X-Content-Type-Options': 'nosniff'
            }
        )
    except Exception as e:
        print(f'❌ Error serving firmware: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/api/firmware/status', methods=['GET'])
def firmware_status():
    """Dashboard calls this to display current firmware info."""
    try:
        conn = get_db_connection()
        if not conn:
            return jsonify({'status': 'error', 'message': 'Database unavailable'}), 500
        
        try:
            cursor = conn.cursor()
            cursor.execute('''
                SELECT version, filename, file_size, checksum, uploaded_at, notes
                FROM firmware_versions
                ORDER BY id DESC LIMIT 5
            ''')
            rows = cursor.fetchall()
            
            versions = []
            for row in rows:
                versions.append({
                    'version': row[0],
                    'filename': row[1],
                    'file_size': row[2],
                    'checksum': row[3],
                    'uploaded_at': row[4],
                    'notes': row[5]
                })
            
            return jsonify({
                'status': 'success',
                'versions': versions,
                'ota_enabled_devices': {k: v for k, v in ota_enabled_devices.items()},
                'latest_version': versions[0]['version'] if versions else None
            }), 200
            
        finally:
            conn.close()
            
    except Exception as e:
        print(f'❌ Error getting firmware status: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500


# ==================== OTA LIVE PROGRESS ENDPOINTS ====================

@app.route('/api/ota/progress', methods=['GET'])
def get_ota_progress():
    """Frontend polls this every 2s to show live OTA status on ESP32.
    
    Returns current OTA state for a device: waiting/checking/downloading/flashing/rebooting/done/failed.
    The 'ota_active' flag tells the frontend whether to show the live progress widget.
    """
    try:
        device_id = request.args.get('device_id', 'ESP32_001')
        state = ota_progress_state.get(device_id)
        
        if not state:
            return jsonify({
                'status': 'success',
                'ota_active': False,
                'ota_status': 'idle',
                'progress': 0,
                'target_version': '',
                'message': 'No OTA in progress',
                'elapsed_seconds': 0
            }), 200
        
        elapsed = int(time.time() - state.get('timestamp', time.time()))
        return jsonify({
            'status': 'success',
            'ota_active': state.get('ota_active', False),
            'ota_status': state.get('ota_status', 'waiting'),
            'progress': state.get('progress', 0),
            'target_version': state.get('target_version', ''),
            'message': state.get('message', ''),
            'elapsed_seconds': elapsed
        }), 200
        
    except Exception as e:
        print(f'❌ Error getting OTA progress: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/api/ota/progress', methods=['POST'])
def update_ota_progress():
    """ESP32 calls this to report its OTA download/flash progress.
    
    Expected JSON: {device_id, ota_status, progress, target_version, message}
    ota_status values: 'checking' | 'downloading' | 'flashing' | 'rebooting' | 'failed'
    Progress 0-100 (percentage of firmware downloaded).
    """
    try:
        data = request.get_json() or {}
        device_id = data.get('device_id', 'ESP32_001')
        ota_status = data.get('ota_status', 'downloading')
        progress = int(data.get('progress', 0))
        target_version = data.get('target_version', '')
        message = data.get('message', '')
        
        # Preserve original start timestamp so elapsed_seconds is accurate
        existing = ota_progress_state.get(device_id, {})
        ts = existing.get('timestamp', time.time())
        
        ota_progress_state[device_id] = {
            'ota_active': ota_status not in ('done', 'failed'),
            'ota_status': ota_status,
            'progress': progress,
            'target_version': target_version,
            'message': message,
            'timestamp': ts
        }
        
        print(f'📡 OTA [{device_id}] {ota_status} {progress}% — {message}')
        return jsonify({'status': 'success'}), 200
        
    except Exception as e:
        print(f'❌ Error updating OTA progress: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500


# ==================== Error Handlers ====================

@app.errorhandler(404)
def not_found(error):
    return jsonify({'error': 'Endpoint not found'}), 404

@app.errorhandler(500)
def internal_error(error):
    print(f'Internal server error: {error}')
    return jsonify({'error': 'Internal server error'}), 500

@app.errorhandler(413)
def request_entity_too_large(error):
    return jsonify({'error': 'File too large'}), 413

@app.errorhandler(400)
def bad_request(error):
    print(f'Bad request: {error}')
    return jsonify({'error': 'Bad request'}), 400

# Handle WebSocket errors gracefully
@socketio.on_error()
def error_handler(e):
    print(f'SocketIO error: {e}')

# Handle connection errors
@socketio.on_error_default
def default_error_handler(e):
    print(f'SocketIO default error: {e}')

# ==================== Main ====================

# Initialize database at module level so it works under both:
# 1. Direct execution (python app.py)
# 2. Gunicorn import (gunicorn app:app)
init_database()

if __name__ == '__main__':
    print('🚀 Starting ESP32 Dashboard Server...')
    print('📊 Dashboard: http://192.168.1.6:5000')  # Local PC IP
    print('🔌 WebSocket: ws://192.168.1.6:5000/socket.io/')
    print('📡 Endpoints:')
    print('   • POST /api/sensor-data (JSON, ~146 bytes)')
    print('   • POST /upload (Binary, ~1-3KB)')  
    print('   • POST /upload-audio (JSON, ~32KB+)')
    print('   • GET  /api/oled-display/get (Pet AI state)')
    print('   • POST /api/pet/inject')
    print('   • POST /api/pet/play-result')
    print('   • POST /api/pet/menu')
    print('')
    
    # Initialize database
    if not init_database():
        print('❌ Database initialization failed. Exiting.')
        exit(1)
    
    try:
        # Run the app with stability-focused configuration
        socketio.run(app, 
            host='0.0.0.0', 
            port=5000, 
            debug=False,  # Disable debug to prevent reloading
            allow_unsafe_werkzeug=True,
            use_reloader=False,  # Prevent duplicate processes
            log_output=False  # Reduce logging overhead
        )
    except KeyboardInterrupt:
        print('\n🛑 Server stopped by user')
    except Exception as e:
        print(f'❌ Server error: {e}')
        print('💡 Check if port 5000 is available and try restarting')

