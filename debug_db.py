#!/usr/bin/env python3
"""Debug database structure and test queries"""

import sqlite3
import os

DB_PATH = 'pet_data.db'

def check_database():
    """Check database structure and content"""
    try:
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        
        print("=" * 60)
        print("CHECKING oled_display_state TABLE")
        print("=" * 60)
        
        # Check table structure
        cursor.execute("PRAGMA table_info(oled_display_state)")
        columns = cursor.fetchall()
        
        print("\n📋 Table Columns:")
        for col in columns:
            print(f"  - {col[1]}: {col[2]}")
        
        # Check row count
        cursor.execute("SELECT COUNT(*) FROM oled_display_state")
        count = cursor.fetchone()[0]
        print(f"\n📊 Row Count: {count}")
        
        # Try to select data
        print("\n📥 Attempting SELECT with all columns...")
        try:
            cursor.execute('''
                SELECT animation_id, animation_name, animation_type, show_home_icon, screen_type, updated_at
                FROM oled_display_state
                WHERE device_id = 'ESP32_001'
            ''')
            result = cursor.fetchone()
            if result:
                print(f"✅ SELECT successful: {result}")
            else:
                print("⚠️ No rows found")
        except sqlite3.Error as e:
            print(f"❌ SELECT failed: {e}")
        
        print("\n" + "=" * 60)
        print("CHECKING pet_state TABLE")
        print("=" * 60)
        
        # Check pet_state table
        cursor.execute("PRAGMA table_info(pet_state)")
        columns = cursor.fetchall()
        
        print("\n📋 Table Columns:")
        for col in columns:
            print(f"  - {col[1]}: {col[2]}")
        
        # Check row count
        cursor.execute("SELECT COUNT(*) FROM pet_state")
        count = cursor.fetchone()[0]
        print(f"\n📊 Row Count: {count}")
        
        # Try to get pet state
        print("\n📥 Attempting get_pet_state query...")
        try:
            cursor.execute('''
                SELECT age, stage, health, hunger, cleanliness, happiness, energy,
                       poop_present, poop_timestamp, current_menu, current_emotion,
                       emotion_expire_at, action_lock, version, digestion_due_time,
                       last_feed_time, last_play_time, last_sleep_time, last_clean_time,
                       last_age_increment
                FROM pet_state
                WHERE device_id = 'ESP32_001'
            ''')
            
            result = cursor.fetchone()
            if result:
                print(f"✅ SELECT successful")
                print(f"   Stage: {result[1]}")
                print(f"   Health: {result[2]}")
                print(f"   Hunger: {result[3]}")
                print(f"   Emotion: {result[10]}")
            else:
                print("⚠️ No pet found")
        except sqlite3.Error as e:
            print(f"❌ SELECT failed: {e}")
        
        conn.close()
        
    except Exception as e:
        print(f"❌ Error: {e}")
        import traceback
        traceback.print_exc()

if __name__ == '__main__':
    check_database()
