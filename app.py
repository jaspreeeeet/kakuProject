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

# AI Vision imports - Google ViT Model
try:
    from PIL import Image
    import numpy as np
    from transformers import pipeline
    import torch
    AI_AVAILABLE = True
    AI_MODE = "FULL"
    print("✅ Google ViT AI Vision model enabled (FULL mode with transformers)")
except ImportError as e:
    print(f"⚠️ FullAI modules not available: {e}")
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
        with step_counter_lock:
            step_count_global += 1
        print(f'     ✅👣 STEP #{step_count_global}! stoss: {stoss:.0f} > barrier: {barrier} | interval: {time_since_last_step:.2f}s')
    return steps_detected

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
    # Reduce overhead
    if 'Content-Type' not in response.headers:
        response.headers['Content-Type'] = 'application/json'
    return response

# Database configuration
DB_PATH = 'sensor_data.db'
ALLOWED_EXTENSIONS = {'jpg', 'jpeg', 'png', 'gif'}

# AI Configuration
if AI_AVAILABLE:
    CACHE_DIR = r"E:\Rajeev\esp 32\esp 32\.cache\huggingface"
    os.environ['HUGGINGFACE_HUB_CACHE'] = CACHE_DIR
    os.makedirs(CACHE_DIR, exist_ok=True)
    
    # Determine device: GPU if available, otherwise CPU
    try:
        AI_DEVICE = 0 if torch.cuda.is_available() else -1
        if torch.cuda.is_available():
            print(f"✅ GPU available: {torch.cuda.get_device_name(0)}")
        else:
            print("⚠️ GPU not available, using CPU for AI analysis")
    except:
        AI_DEVICE = -1
        print("⚠️ Using CPU for AI analysis")
    
    # Initialize AI model (lazy loading)
    ai_classifier = None

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

# AI Analysis Function with fallback
def analyze_image_with_ai(image_path):
    """Analyze image using AI models or basic image analysis as fallback"""
    global ai_classifier
    
    if not AI_AVAILABLE:
        return "AI analysis not available - missing dependencies"
    
    try:
        if AI_MODE == "FULL":
            # Full AI Model Analysis (Google ViT)
            if ai_classifier is None:
                print("Loading Google ViT vision model...")
                ai_classifier = pipeline(
                    "image-classification",
                    model="google/vit-base-patch16-224",
                    device=AI_DEVICE
                )
                print("Google ViT model loaded successfully")
            
            # Load and analyze image
            image = Image.open(image_path)
            if image.mode != 'RGB':
                image = image.convert('RGB')
            
            # Get AI predictions
            results = ai_classifier(image, top_k=5)
            
            # Generate natural caption 
            top_result = results[0]
            confidence = top_result['score'] * 100
            main_label = top_result['label']
            
            # Check for people-related content
            people_keywords = ['people', 'person', 'group', 'crowd', 'team', 'family', 'human', 'face', 'portrait']
            people_detected = any(keyword in result['label'].lower() for result in results for keyword in people_keywords)
            
            # Generate natural, descriptive caption
            if people_detected:
                caption = f"This image shows a group of people (detected with {confidence:.1f}% confidence)"
            else:
                main_label_clean = main_label.replace('_', ' ').replace('-', ' ')
                caption = f"This image shows {main_label_clean} (detected with {confidence:.1f}% confidence)"
            
            return caption
            
        elif AI_MODE == "BASIC":
            # Basic Image Analysis (PIL + Visual Features)
            image = Image.open(image_path)
            if image.mode != 'RGB':
                image = image.convert('RGB')
            
            # Get image properties
            width, height = image.size
            img_array = np.array(image)
            
            # Basic color analysis
            mean_colors = np.mean(img_array, axis=(0, 1))
            color_variance = np.var(img_array.reshape(-1, 3), axis=0)
            total_variance = np.sum(color_variance)
            brightness = np.mean(img_array)
            
            # Basic feature detection
            aspect_ratio = width / height
            
            # Generate descriptive caption based on visual features
            caption_parts = []
            
            # Resolution description
            if width * height > 100000:
                caption_parts.append("high-resolution")
            else:
                caption_parts.append("compact")
            
            # Color description
            if brightness > 200:
                caption_parts.append("bright")
            elif brightness < 80:
                caption_parts.append("dark")
            else:
                caption_parts.append("well-lit")
            
            # Orientation
            if aspect_ratio > 1.5:
                caption_parts.append("landscape-oriented")
            elif aspect_ratio < 0.7:
                caption_parts.append("portrait-oriented")
            else:
                caption_parts.append("square-oriented")
            
            # Color richness
            if total_variance > 8000:
                caption_parts.append("colorful scene")
            elif total_variance > 3000:
                caption_parts.append("moderately colorful image")
            else:
                caption_parts.append("simple colored image")
            
            # Dominant color
            r, g, b = mean_colors
            if r > g and r > b:
                caption_parts.append("with reddish tones")
            elif g > r and g > b:
                caption_parts.append("with greenish tones")
            elif b > r and b > g:
                caption_parts.append("with bluish tones")
            
            caption = f"This is a {' '.join(caption_parts[:4])} captured from ESP32 camera"
            
            # Add technical details
            caption += f" (Resolution: {width}×{height}, Brightness: {brightness:.0f}/255)"
            
            return caption
    
    except Exception as e:
        print(f"AI Analysis error: {e}")
        import traceback
        traceback.print_exc()
        return f"Image analysis failed: {str(e)[:100]}..."

# ❌ DISABLED: Background AI analysis (file-based, not compatible with database-only storage)
# ================= BACKGROUND AI ANALYSIS (NON-BLOCKING) =================
# def analyze_and_store_image(filepath, filename):
#     """Background task: Run AI analysis and store result without blocking server"""
#     try:
#         print(f"🤖 [BACKGROUND] Starting AI analysis for {filename}...")
#         ai_caption = analyze_image_with_ai(filepath)
#         
#         # Store in database
#         with db_lock:
#             conn = get_db_connection()
#             if conn:
#                 try:
#                     cursor = conn.cursor()
#                     cursor.execute('''
#                         UPDATE sensor_readings 
#                         SET image_filename = ?, ai_caption = ?
#                         WHERE id = (SELECT MAX(id) FROM sensor_readings WHERE accel_x IS NOT NULL)
#                     ''', (filename, ai_caption))
#                     
#                     if cursor.rowcount == 0:
#                         cursor.execute('''
#                             INSERT INTO sensor_readings (device_id, image_filename, ai_caption, timestamp)
#                             VALUES (?, ?, ?, CURRENT_TIMESTAMP)
#                         ''', ('ESP32_CAM', filename, ai_caption))
#                     
#                     conn.commit()
#                     print(f"✅ [BG] Stored AI caption in database")
#                 except sqlite3.Error as e:
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
    """Get thread-safe database connection with error handling"""
    try:
        conn = sqlite3.connect(DB_PATH, timeout=30, isolation_level=None)
        conn.execute("PRAGMA journal_mode=WAL;")
        conn.execute("PRAGMA synchronous=NORMAL;")
        conn.execute("PRAGMA cache_size=10000;")
        conn.execute("PRAGMA temp_store=memory;")
        return conn
    except sqlite3.Error as e:
        print(f"Database connection error: {e}")
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
                    calibrated_az REAL
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
            
            # 🐾 Create pet_state table for Tamagotchi AI cloud brain
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS pet_state (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT DEFAULT 'ESP32_001',
                    
                    age INTEGER DEFAULT 0,
                    stage TEXT DEFAULT 'INFANT',
                    
                    health INTEGER DEFAULT 100,
                    hunger INTEGER DEFAULT 0,
                    cleanliness INTEGER DEFAULT 100,
                    happiness INTEGER DEFAULT 100,
                    energy INTEGER DEFAULT 100,
                    
                    poop_present BOOLEAN DEFAULT 0,
                    poop_timestamp DATETIME,
                    digestion_due_time DATETIME,
                    
                    current_menu TEXT DEFAULT 'MAIN',
                    current_emotion TEXT DEFAULT 'IDLE',
                    emotion_expire_at DATETIME,
                    
                    action_lock BOOLEAN DEFAULT 0,
                    version INTEGER DEFAULT 0,
                    
                    last_feed_time DATETIME,
                    last_play_time DATETIME,
                    last_sleep_time DATETIME,
                    last_clean_time DATETIME,
                    last_age_increment DATETIME DEFAULT CURRENT_TIMESTAMP,
                    
                    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
                )
            ''')
            print("✅ Created pet_state table")
            
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
            
            # Fetch current state with version
            cursor.execute('''
                SELECT id, version, action_lock, emotion_expire_at,
                       age, stage, health, hunger, cleanliness, happiness, energy,
                       poop_present, poop_timestamp, digestion_due_time,
                       current_menu, current_emotion,
                       last_feed_time, last_play_time, last_sleep_time, last_clean_time,
                       last_age_increment
                FROM pet_state
                WHERE device_id = ?
            ''', (device_id,))
            
            result = cursor.fetchone()
            if not result:
                print(f"❌ No pet state found for {device_id}")
                return None
            
            # Build current state
            current_state = {
                'id': result[0],
                'version': result[1],
                'action_lock': result[2],
                'emotion_expire_at': result[3],
                'age': result[4],
                'stage': result[5],
                'health': result[6],
                'hunger': result[7],
                'cleanliness': result[8],
                'happiness': result[9],
                'energy': result[10],
                'poop_present': result[11],
                'poop_timestamp': result[12],
                'digestion_due_time': result[13],
                'current_menu': result[14],
                'current_emotion': result[15],
                'last_feed_time': result[16],
                'last_play_time': result[17],
                'last_sleep_time': result[18],
                'last_clean_time': result[19],
                'last_age_increment': result[20]
            }
            
            # Merge updates
            new_state = {**current_state, **update_fields}
            new_state['version'] = current_state['version'] + 1
            new_state['updated_at'] = datetime.now().isoformat()
            
            # Update database
            cursor.execute('''
                UPDATE pet_state
                SET version = ?, age = ?, stage = ?, health = ?, hunger = ?,
                    cleanliness = ?, happiness = ?, energy = ?,
                    poop_present = ?, poop_timestamp = ?, digestion_due_time = ?,
                    current_menu = ?, current_emotion = ?, emotion_expire_at = ?,
                    action_lock = ?,
                    last_feed_time = ?, last_play_time = ?, last_sleep_time = ?, last_clean_time = ?,
                    last_age_increment = ?, updated_at = CURRENT_TIMESTAMP
                WHERE id = ? AND version = ?
            ''', (
                new_state['version'], new_state['age'], new_state['stage'],
                new_state['health'], new_state['hunger'], new_state['cleanliness'],
                new_state['happiness'], new_state['energy'],
                new_state['poop_present'], new_state['poop_timestamp'], new_state['digestion_due_time'],
                new_state['current_menu'], new_state['current_emotion'], new_state['emotion_expire_at'],
                new_state['action_lock'],
                new_state['last_feed_time'], new_state['last_play_time'],
                new_state['last_sleep_time'], new_state['last_clean_time'],
                new_state['last_age_increment'],
                current_state['id'], current_state['version']
            ))
            
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
    """Get current pet state safely"""
    with db_lock:
        conn = get_db_connection()
        if not conn:
            return None
        
        try:
            cursor = conn.cursor()
            cursor.execute('''
                SELECT age, stage, health, hunger, cleanliness, happiness, energy,
                       poop_present, poop_timestamp, current_menu, current_emotion,
                       emotion_expire_at, action_lock, version, digestion_due_time,
                       last_feed_time, last_play_time, last_sleep_time, last_clean_time,
                       last_age_increment
                FROM pet_state
                WHERE device_id = ?
            ''', (device_id,))
            
            result = cursor.fetchone()
            if not result:
                return None
            
            return {
                'age': result[0],
                'stage': result[1],
                'health': result[2],
                'hunger': result[3],
                'cleanliness': result[4],
                'happiness': result[5],
                'energy': result[6],
                'poop_present': bool(result[7]),
                'poop_timestamp': result[8],
                'current_menu': result[9],
                'current_emotion': result[10],
                'emotion_expire_at': result[11],
                'action_lock': bool(result[12]),
                'version': result[13],
                'digestion_due_time': result[14],
                'last_feed_time': result[15],
                'last_play_time': result[16],
                'last_sleep_time': result[17],
                'last_clean_time': result[18],
                'last_age_increment': result[19]
            }
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
    if state['health'] < 40:
        return 'SICK'
    
    if state['poop_present']:
        # Check poop age
        if state.get('poop_timestamp'):
            from datetime import datetime, timedelta
            poop_time = datetime.fromisoformat(state['poop_timestamp']) if isinstance(state['poop_timestamp'], str) else state['poop_timestamp']
            if poop_time and (datetime.now() - poop_time) > timedelta(minutes=15):
                return 'SICK'  # Ignored poop → sick
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
    Background thread that runs every 60 seconds
    Handles hunger, digestion, poop, sickness, age progression, and emotion updates
    """
    while True:
        try:
            time.sleep(60)  # Run every 60 seconds
            
            device_id = 'ESP32_001'
            state = get_pet_state(device_id)
            
            if not state:
                print("⚠️ Pet engine: No pet state found")
                continue
            
            # Skip if action is locked
            if state['action_lock']:
                print("🔒 Pet engine: Skipping update (action locked)")
                continue
            
            print(f"\n🐾 Pet Engine Cycle - Age: {state['age']} | Stage: {state['stage']} | Health: {state['health']}")
            
            updates = {}
            
            # 1️⃣ AGE PROGRESSION (every 24 hours)
            if state.get('last_age_increment'):
                from datetime import datetime, timedelta
                last_age_time = datetime.fromisoformat(state['last_age_increment']) if isinstance(state['last_age_increment'], str) else state['last_age_increment']
                if last_age_time and (datetime.now() - last_age_time) > timedelta(hours=24):
                    updates['age'] = state['age'] + 1
                    updates['last_age_increment'] = datetime.now().isoformat()
                    print(f"🎂 Age increased: {state['age']} → {updates['age']}")
                    
                    # Update stage based on age
                    new_age = updates['age']
                    if new_age <= 5:
                        updates['stage'] = 'INFANT'
                    elif new_age <= 10:
                        updates['stage'] = 'CHILD'
                    elif new_age <= 17:
                        updates['stage'] = 'ADULT'
                    elif new_age <= 21:
                        updates['stage'] = 'OLD'
                    else:
                        updates['stage'] = 'END'
                    
                    print(f"📊 Stage updated: {state['stage']} → {updates['stage']}")
            
            # 2️⃣ HUNGER ENGINE (every 30 minutes)
            hunger_increase = 0
            current_stage = updates.get('stage', state['stage'])
            
            if current_stage == 'INFANT':
                hunger_increase = 15
            elif current_stage == 'CHILD':
                hunger_increase = 10
            elif current_stage == 'ADULT':
                hunger_increase = 8
            elif current_stage == 'OLD':
                hunger_increase = 12
            
            # Apply hunger increase (scaled for 60-second intervals)
            updates['hunger'] = min(100, state['hunger'] + (hunger_increase / 30))
            
            # 3️⃣ DIGESTION & POOP ENGINE
            if state.get('digestion_due_time'):
                from datetime import datetime
                due_time = datetime.fromisoformat(state['digestion_due_time']) if isinstance(state['digestion_due_time'], str) else state['digestion_due_time']
                if due_time and datetime.now() > due_time and not state['poop_present']:
                    updates['poop_present'] = 1
                    updates['poop_timestamp'] = datetime.now().isoformat()
                    updates['digestion_due_time'] = None
                    print("💩 Poop appeared (digestion complete)")
            
            # Check poop age for health penalty
            if state['poop_present'] and state.get('poop_timestamp'):
                from datetime import datetime, timedelta
                poop_time = datetime.fromisoformat(state['poop_timestamp']) if isinstance(state['poop_timestamp'], str) else state['poop_timestamp']
                if poop_time and (datetime.now() - poop_time) > timedelta(minutes=15):
                    updates['health'] = max(0, state['health'] - 10)
                    print(f"🤢 Poop ignored >15min → Health penalty: {state['health']} → {updates['health']}")
            
            # 4️⃣ ENERGY DECAY
            updates['energy'] = max(0, state['energy'] - 2)
            
            # 5️⃣ SICKNESS (OLD age random sickness)
            if current_stage == 'OLD':
                import random
                if random.random() < 0.01:  # 1% chance per cycle
                    updates['health'] = max(0, state['health'] - 5)
                    print(f"🤒 Random sickness (OLD age): Health {state['health']} → {updates['health']}")
            
            # 6️⃣ EMOTION PRIORITY UPDATE
            merged_state = {**state, **updates}
            new_emotion = get_emotion_priority(merged_state)
            
            # Only update emotion if not locked
            if not state.get('emotion_expire_at') or datetime.fromisoformat(state['emotion_expire_at']) <= datetime.now():
                updates['current_emotion'] = new_emotion
                print(f"😊 Emotion updated: {state['current_emotion']} → {new_emotion}")
            
            # Apply updates atomically
            if updates:
                result = update_pet_state_atomic(device_id, updates)
                if result:
                    print(f"✅ Pet engine cycle complete")
                    
                    # Broadcast update to frontend
                    socketio.start_background_task(lambda: socketio.emit('pet_state_update', {
                        'stage': result['stage'],
                        'emotion': result['current_emotion'],
                        'health': result['health'],
                        'hunger': result['hunger'],
                        'cleanliness': result['cleanliness'],
                        'happiness': result['happiness'],
                        'energy': result['energy'],
                        'poop_present': result['poop_present'],
                        'age': result['age']
                    }))
                else:
                    print("❌ Pet engine update failed")
            else:
                print("No updates needed")
                    
        except Exception as e:
            print(f"❌ Pet engine error: {e}")
            import traceback
            traceback.print_exc()

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
        
        # 👣 COMPUTE STEP COUNT ON SERVER
        import time
        current_time = time.time()
        
        # NEW: Process sensor batch if available (multiple readings from ESP32)
        total_steps_batch = 0
        
        # Reduced logging for performance
        if data.get('sensor_batch') and data['sensor_batch'].get('readings'):
            readings = data['sensor_batch']['readings']
            
            for idx, reading in enumerate(readings):
                batch_accel_x = reading.get('accel_x', 0)
                batch_accel_y = reading.get('accel_y', 0)
                batch_accel_z = reading.get('accel_z', 0)
                batch_time = current_time + (idx * 0.1)  # Approximate timing based on index
                
                steps_in_reading = detect_steps(batch_accel_x, batch_accel_y, batch_accel_z, batch_time)
                total_steps_batch += steps_in_reading
        else:
            # Fall back to single reading detection
            steps_in_reading = detect_steps(accel_x, accel_y, accel_z, current_time)
            total_steps_batch = steps_in_reading
        
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
                        'sound_data': data.get('sound_data', 0)
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
        
        # NEW: FEED THE PET - The captured frame IS the food!
        # Reduce hunger immediately when image is received
        state = get_pet_state(device_id)
        if state:
            from datetime import datetime, timedelta
            
            # Feed logic (same as /api/pet/feed)
            updates = {
                'hunger': max(0, state['hunger'] - 40),
                'last_feed_time': datetime.now().isoformat(),
                'digestion_due_time': (datetime.now() + timedelta(minutes=30)).isoformat(),
                'current_emotion': 'EATING',
                'emotion_expire_at': (datetime.now() + timedelta(seconds=3)).isoformat()
            }
            
            result = update_pet_state_atomic(device_id, updates)
            
            if result:
                print(f"🍔 Pet auto-fed via image upload: hunger {state['hunger']} → {result['hunger']}")
        
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
                       CASE WHEN camera_image IS NOT NULL THEN 1 ELSE 0 END as has_image,
                       CASE WHEN audio_data IS NOT NULL THEN 1 ELSE 0 END as has_audio
                FROM sensor_readings 
                ORDER BY timestamp DESC 
                LIMIT ?
            ''', (limit,))
        else:
            cursor.execute('''
                SELECT id, timestamp, accel_x, accel_y, accel_z, 
                       gyro_x, gyro_y, gyro_z, mic_level, sound_data, image_filename,
                       CASE WHEN camera_image IS NOT NULL THEN 1 ELSE 0 END as has_image,
                       0 as has_audio
                FROM sensor_readings 
                ORDER BY timestamp DESC 
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
            ORDER BY timestamp DESC 
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

@app.route('/api/pet/feed', methods=['POST'])
def pet_feed():
    """Feed the pet - reduces hunger, schedules digestion/poop"""
    try:
        data = request.get_json() or {}
        device_id = data.get('device_id', 'ESP32_001')
        
        state = get_pet_state(device_id)
        if not state:
            return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
        
        from datetime import datetime, timedelta
        
        # Set action lock
        updates = {
            'action_lock': 1
        }
        update_pet_state_atomic(device_id, updates)
        
        # Feed logic
        updates = {
            'hunger': max(0, state['hunger'] - 40),
            'last_feed_time': datetime.now().isoformat(),
            'digestion_due_time': (datetime.now() + timedelta(minutes=30)).isoformat(),
            'current_emotion': 'EATING',
            'emotion_expire_at': (datetime.now() + timedelta(seconds=3)).isoformat(),
            'action_lock': 0
        }
        
        result = update_pet_state_atomic(device_id, updates)
        
        if result:
            print(f"🍔 Pet fed: hunger {state['hunger']} → {result['hunger']}")
            return jsonify({
                'status': 'success',
                'message': 'Pet fed successfully',
                'hunger': result['hunger'],
                'emotion': result['current_emotion'],
                'digestion_due': result['digestion_due_time']
            }), 200
        else:
            return jsonify({'status': 'error', 'message': 'Failed to feed pet'}), 500
            
    except Exception as e:
        print(f'❌ Error feeding pet: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/pet/clean', methods=['POST'])
def pet_clean():
    """Clean the pet - removes poop, restores cleanliness, boosts health"""
    try:
        data = request.get_json() or {}
        device_id = data.get('device_id', 'ESP32_001')
        
        state = get_pet_state(device_id)
        if not state:
            return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
        
        from datetime import datetime, timedelta
        
        if not state['poop_present']:
            return jsonify({
                'status': 'success',
                'message': 'Already clean',
                'emotion': 'IDLE'
            }), 200
        
        # Set action lock
        updates = {'action_lock': 1}
        update_pet_state_atomic(device_id, updates)
        
        # Clean logic
        updates = {
            'poop_present': 0,
            'poop_timestamp': None,
            'cleanliness': 100,
            'health': min(100, state['health'] + 5),
            'last_clean_time': datetime.now().isoformat(),
            'current_emotion': 'CLEAN_SUCCESS',
            'emotion_expire_at': (datetime.now() + timedelta(seconds=3)).isoformat(),
            'action_lock': 0
        }
        
        result = update_pet_state_atomic(device_id, updates)
        
        if result:
            print(f"🧹 Pet cleaned: health {state['health']} → {result['health']}")
            return jsonify({
                'status': 'success',
                'message': 'Pet cleaned successfully',
                'cleanliness': result['cleanliness'],
                'health': result['health'],
                'emotion': result['current_emotion']
            }), 200
        else:
            return jsonify({'status': 'error', 'message': 'Failed to clean pet'}), 500
            
    except Exception as e:
        print(f'❌ Error cleaning pet: {e}')
        return jsonify({'status': 'error', 'message': str(e)}), 500

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
        
        # Injection logic
        updates = {
            'health': min(100, state['health'] + 20),
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
                 device_orientation, orientation_confidence, calibrated_ax, calibrated_ay, calibrated_az, step_count)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
                data.get('step_count', 0)
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

@app.route('/api/oled-display/get', methods=['GET'])
def get_oled_display():
    """ESP32 polls this endpoint to get what animation to display on OLED
    
    PRIORITY ORDER:
    1. Recent device startup reset (within 5 seconds) → ALWAYS return INFANT
    2. Manual button selection from web UI (oled_display_state table)
    3. AI Tamagotchi pet state (pet_state table with health/hunger)
    4. Default fallback
    
    Includes backward compatibility with animation_id field
    """
    try:
        device_id = request.args.get('device_id', 'ESP32_001')
        
        # FIRST: Check if device just booted (reset timestamp recent)
        startup_reset = False
        with db_lock:
            conn = get_db_connection()
            if conn:
                try:
                    cursor = conn.cursor()
                    cursor.execute('''
                        SELECT animation_id, animation_name, updated_at, updated_by
                        FROM oled_display_state
                        WHERE device_id = ?
                    ''', (device_id,))
                    result = cursor.fetchone()
                    if result:
                        updated_by = result[3]  # Check who updated it
                        updated_at = result[2]
                        
                        # If updated by device_startup, check if it's recent (within 5 seconds)
                        if updated_by == 'device_startup' and updated_at:
                            try:
                                from datetime import datetime, timedelta
                                updated_time = datetime.fromisoformat(updated_at)
                                current_time = datetime.utcnow()
                                time_diff = (current_time - updated_time).total_seconds()
                                
                                if time_diff < 5:  # Within 5 seconds of startup
                                    startup_reset = True
                                    print(f'🔄 INFANT locked - device startup {time_diff:.1f}s ago')
                                    return jsonify({
                                        'status': 'success',
                                        'animation_id': 0,
                                        'animation_name': 'INFANT',
                                        'animation_type': 'pet',
                                        'stage': 'INFANT',
                                        'mode': 'STARTUP_RESET',
                                        'show_home_icon': False,
                                        'show_food_icon': False,
                                        'show_poop_icon': False,
                                        'screen_type': 'MAIN',
                                        'message': f'Device startup - INFANT locked for {5 - time_diff:.1f}s'
                                    }), 200
                            except Exception as e:
                                print(f'⚠️  Timestamp parse error: {e}')
                finally:
                    conn.close()
        
        # SECOND: Check if user manually selected an animation via web UI buttons
        manual_selection = None
        with db_lock:
            conn = get_db_connection()
            if conn:
                try:
                    cursor = conn.cursor()
                    cursor.execute('''
                        SELECT animation_id, animation_name, animation_type, show_home_icon, show_food_icon, show_poop_icon, screen_type, updated_at
                        FROM oled_display_state
                        WHERE device_id = ?
                    ''', (device_id,))
                    result = cursor.fetchone()
                    if result:
                        manual_selection = {
                            'animation_id': result[0],
                            'animation_name': result[1],
                            'animation_type': result[2],
                            'show_home_icon': result[3],
                            'show_food_icon': result[4],
                            'show_poop_icon': result[5],
                            'screen_type': result[6],
                            'updated_at': result[7]
                        }
                finally:
                    conn.close()
        
        # If manual selection exists AND not during startup, use it (user pressed a button!)
        if manual_selection and not startup_reset:
            print(f'📡 OLED: MANUAL SELECTION → {manual_selection["animation_name"]} (ID: {manual_selection["animation_id"]})')
            return jsonify({
                'status': 'success',
                'animation_id': manual_selection['animation_id'],
                'animation_name': manual_selection['animation_name'],
                'animation_type': manual_selection['animation_type'],
                'stage': manual_selection['animation_name'],
                'mode': 'MANUAL',
                'show_home_icon': bool(manual_selection.get('show_home_icon')) if manual_selection.get('show_home_icon') is not None else False,
                'show_food_icon': bool(manual_selection.get('show_food_icon')) if manual_selection.get('show_food_icon') is not None else False,
                'show_poop_icon': bool(manual_selection.get('show_poop_icon')) if manual_selection.get('show_poop_icon') is not None else False,
                'screen_type': manual_selection.get('screen_type') or 'MAIN',
                'message': f'Manual selection: {manual_selection["animation_name"]}'
            }), 200
        
        # THIRD: Fall back to AI pet state
        pet = get_pet_state(device_id)
        
        if not pet:
            # Fallback if no pet state exists
            return jsonify({
                'status': 'success',
                'animation_id': 1,
                'stage': 'CHILD',
                'emotion': 'IDLE',
                'current_emotion': 'IDLE',
                'current_menu': 'MAIN',
                'health': 100,
                'hunger': 0,
                'cleanliness': 100,
                'happiness': 100,
                'energy': 100,
                'poop_present': False,
                'show_home_icon': True,
                'show_food_icon': False,
                'show_poop_icon': False,
                'screen_type': 'MAIN',
                'mode': 'DEFAULT',
                'message': 'Default pet state'
            }), 200
        
        # Map stage to animation_id for backward compatibility
        stage_to_id = {
            'INFANT': 0,
            'CHILD': 1,
            'ADULT': 2,
            'OLD': 3,
            'END': 3
        }
        
        animation_id = stage_to_id.get(pet['stage'], 1)
        
        # NEW: Menu state management
        # Menu is controlled by USER ONLY (frontend buttons or camera cover)
        # NO auto-switching based on hunger/poop
        current_menu = pet['current_menu']
        play_eating = False
        play_cleaning = False
        
        # Only handle eating/cleaning logic when user is already on menu
        
        if pet['hunger'] <= 50 and current_menu == 'FOOD_MENU':
            # Pet is no longer hungry, trigger eating animation on FOOD_MENU
            play_eating = True  # Trigger eating animation on FOOD_MENU
            # DON'T return to MAIN yet - let eating animation play on FOOD_MENU
            # Current menu stays FOOD_MENU to show eating animation
            # After animation, next poll will return to MAIN
            with db_lock:
                conn = get_db_connection()
                if conn:
                    try:
                        cursor = conn.cursor()
                        # Set emotion to EATING for animation trigger
                        cursor.execute('UPDATE pet_state SET current_emotion = ? WHERE device_id = ?', ('EATING', device_id))
                        conn.commit()
                        # Re-fetch pet state to get updated emotion
                        pet = get_pet_state(device_id)
                    except Exception as e:
                        print(f'Error updating emotion: {e}')
                    finally:
                        conn.close()
        
        elif pet['current_emotion'] == 'EATING' and current_menu == 'FOOD_MENU':
            # Eating animation finished, return to MAIN with IDLE emotion
            current_menu = 'MAIN'
            with db_lock:
                conn = get_db_connection()
                if conn:
                    try:
                        cursor = conn.cursor()
                        cursor.execute('UPDATE pet_state SET current_menu = ?, current_emotion = ? WHERE device_id = ?', 
                                     ('MAIN', 'IDLE', device_id))
                        conn.commit()
                        # Re-fetch pet state to get updated values
                        pet = get_pet_state(device_id)
                    except Exception as e:
                        print(f'Error updating menu: {e}')
                    finally:
                        conn.close()
        
        elif not pet['poop_present'] and current_menu == 'TOILET_MENU':
            # Pet is clean, return to MAIN with IDLE emotion
            current_menu = 'MAIN'
            with db_lock:
                conn = get_db_connection()
                if conn:
                    try:
                        cursor = conn.cursor()
                        cursor.execute('UPDATE pet_state SET current_menu = ?, current_emotion = ? WHERE device_id = ?', 
                                     ('MAIN', 'IDLE', device_id))
                        conn.commit()
                        # Re-fetch pet state to get updated values
                        pet = get_pet_state(device_id)
                    except Exception as e:
                        print(f'Error updating menu: {e}')
                    finally:
                        conn.close()
        
        print(f'📡 OLED: AI PET STATE → {pet["stage"]} | {pet["current_emotion"]} | Menu: {current_menu} | H:{pet["health"]} F:{pet["hunger"]}')
        
        return jsonify({
            'status': 'success',
            # Backward compatibility
            'animation_id': animation_id,
            'animation_name': pet['stage'],
            
            # Full pet state for AI Tamagotchi
            'stage': pet['stage'],
            'emotion': pet['current_emotion'],
            'current_emotion': pet['current_emotion'],
            'current_menu': current_menu,
            'health': pet['health'],
            'hunger': pet['hunger'],
            'cleanliness': pet['cleanliness'],
            'happiness': pet['happiness'],
            'energy': pet['energy'],
            'poop_present': pet['poop_present'],
            'age': pet['age'],
            'mode': 'AUTOMATIC',  # NEW: AI mode is always AUTOMATIC
            'is_hungry': pet['hunger'] > 70,  # NEW: Boolean flag for conditional camera send
            'show_home_icon': True,
            'show_food_icon': pet['hunger'] > 70,  # Show food icon when hungry
            'show_poop_icon': pet['poop_present'],  # Show poop icon when poop present
            'screen_type': current_menu,
            'play_eating_animation': play_eating,  # Trigger eating animation when fed
            'play_cleaning_animation': play_cleaning,  # Trigger cleaning when cleaned
            'message': f'Pet: {pet["stage"]} | Emotion: {pet["current_emotion"]}'
        }), 200
    
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
                    ORDER BY timestamp DESC
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

if __name__ == '__main__':
    print('🚀 Starting ESP32 Dashboard Server...')
    print('📊 Dashboard: http://192.168.1.6:5000')  # Local PC IP
    print('🔌 WebSocket: ws://192.168.1.6:5000/socket.io/')
    print('📡 Endpoints:')
    print('   • POST /api/sensor-data (JSON, ~146 bytes)')
    print('   • POST /upload (Binary, ~1-3KB)')  
    print('   • POST /upload-audio (JSON, ~32KB+)')
    print('   • GET  /api/oled-display/get (Pet AI state)')
    print('   • POST /api/pet/feed')
    print('   • POST /api/pet/clean')
    print('   • POST /api/pet/inject')
    print('   • POST /api/pet/play-result')
    print('   • POST /api/pet/menu')
    print('')
    
    # Initialize database before starting server
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
