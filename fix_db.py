#!/usr/bin/env python3
"""Fix missing database tables"""

import sqlite3

DB_PATH = 'pet_data.db'

def fix_database():
    """Create missing tables"""
    try:
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        
        print("🔧 Fixing database...")
        
        # Create oled_display_state table if it doesn't exist
        print("\n1️⃣ Creating oled_display_state table...")
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS oled_display_state (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT DEFAULT 'ESP32_001',
                animation_type TEXT DEFAULT 'pet',
                animation_id INTEGER DEFAULT 1,
                animation_name TEXT DEFAULT 'CHILD',
                show_home_icon BOOLEAN DEFAULT 0,
                screen_type TEXT DEFAULT 'MAIN',
                updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                updated_by TEXT DEFAULT 'web_ui'
            )
        ''')
        print("✅ oled_display_state table created/verified")
        
        # Initialize default OLED state if not exists
        cursor.execute('SELECT COUNT(*) FROM oled_display_state')
        if cursor.fetchone()[0] == 0:
            cursor.execute('''
                INSERT INTO oled_display_state 
                (device_id, animation_type, animation_id, animation_name, updated_by)
                VALUES (?, ?, ?, ?, ?)
            ''', ('ESP32_001', 'pet', 1, 'CHILD', 'system_init'))
            print("✅ Initialized default OLED display state")
        
        # Create pet_state table if it doesn't exist
        print("\n2️⃣ Creating pet_state table...")
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
        print("✅ pet_state table created/verified")
        
        # Initialize default pet state if not exists
        cursor.execute('SELECT COUNT(*) FROM pet_state')
        if cursor.fetchone()[0] == 0:
            cursor.execute('''
                INSERT INTO pet_state 
                (device_id, age, stage, health, hunger, cleanliness, happiness, energy,
                 current_menu, current_emotion, last_age_increment)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)
            ''', ('ESP32_001', 0, 'INFANT', 100, 0, 100, 100, 100, 'MAIN', 'IDLE'))
            print("✅ Initialized default pet state")
        
        conn.commit()
        conn.close()
        
        print("\n✅ Database fixed successfully!")
        
    except Exception as e:
        print(f"❌ Error: {e}")
        import traceback
        traceback.print_exc()

if __name__ == '__main__':
    fix_database()
