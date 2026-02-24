#!/usr/bin/env python3
"""Check production database (sensor_data.db)"""

import sqlite3

DB_PATH = 'sensor_data.db'

conn = sqlite3.connect(DB_PATH)
cursor = conn.cursor()

print("="*60)
print("PRODUCTION DATABASE: sensor_data.db")
print("="*60)

# Check pet_state table
try:
    cursor.execute('''
        SELECT device_id, stage, health, hunger, happiness, current_emotion, last_feed_time
        FROM pet_state 
        WHERE device_id = ?
    ''', ('ESP32_001',))
    result = cursor.fetchone()
    
    if result:
        print("\n🐾 Pet State (ESP32_001):")
        print(f"  Device: {result[0]}")
        print(f"  Stage: {result[1]}")
        print(f"  Health: {result[2]}")
        print(f"  Hunger: {result[3]}")
        print(f"  Happiness: {result[4]}")
        print(f"  Emotion: {result[5]}")
        print(f"  Last Feed: {result[6]}")
    else:
        print("\n❌ No pet found with device_id = ESP32_001")
        
except Exception as e:
    print(f"\n❌ Error: {e}")

conn.close()
print("="*60)
