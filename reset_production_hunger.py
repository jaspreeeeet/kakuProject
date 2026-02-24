#!/usr/bin/env python3
"""
Reset production server hunger via API call
Simulates a feeding action to reduce hunger on Cloud Run server
"""

import requests
import io
from PIL import Image
import time

# Production server URL
SERVER_URL = "https://kakuproject-90943350924.asia-south1.run.app"

def create_dummy_image():
    """Create a small dummy image for feeding"""
    img = Image.new('RGB', (100, 100), color=(73, 109, 137))
    img_bytes = io.BytesIO()
    img.save(img_bytes, format='JPEG', quality=50)
    img_bytes.seek(0)
    return img_bytes.getvalue()

def feed_pet_on_server():
    """Send feeding request to production server"""
    print("="*60)
    print("RESETTING PRODUCTION SERVER HUNGER")
    print("="*60)
    
    try:
        # Create dummy image
        print("\n📸 Creating dummy image...")
        image_data = create_dummy_image()
        
        # Upload to server with feeding header
        print(f"📤 Uploading to {SERVER_URL}/upload...")
        headers = {
            'X-Feeding-Action': 'true'
        }
        
        response = requests.post(
            f"{SERVER_URL}/upload",
            data=image_data,
            headers=headers,
            timeout=10
        )
        
        if response.status_code == 200:
            print("✅ Upload successful!")
            result = response.json()
            print(f"\n📊 Server Response:")
            print(f"  Status: {result.get('status')}")
            print(f"  Pet Fed: {result.get('pet_fed')}")
            print(f"  Hunger Reduced: {result.get('hunger_reduced')}")
            print(f"  Image ID: {result.get('image_id')}")
            
            # Wait and check new state
            time.sleep(2)
            print(f"\n🔄 Checking new pet state...")
            state_response = requests.get(f"{SERVER_URL}/api/oled-display/get")
            if state_response.status_code == 200:
                state = state_response.json()
                print(f"\n🐾 Updated Pet State:")
                print(f"  Stage: {state.get('stage')}")
                print(f"  Health: {state.get('health')}")
                print(f"  Hunger: {state.get('hunger')} ← Should be reduced by 40")
                print(f"  Emotion: {state.get('current_emotion')}")
                print(f"  Is Hungry: {state.get('is_hungry')}")
            
        else:
            print(f"❌ Upload failed: {response.status_code}")
            print(f"Response: {response.text}")
            
    except requests.exceptions.ConnectionError:
        print("❌ Connection failed! Is server running?")
    except Exception as e:
        print(f"❌ Error: {e}")
    
    print("="*60)

if __name__ == "__main__":
    feed_pet_on_server()
