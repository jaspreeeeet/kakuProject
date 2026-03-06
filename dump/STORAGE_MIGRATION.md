# Storage Migration: Database-Only Image Storage

## 📋 Summary
**Date:** December 2024  
**Purpose:** Migrate from dual storage (filesystem + database) to database-only storage for images  
**Status:** ✅ Complete

## 🎯 Changes Made

### 1. **Disabled File System Image Storage**
- **Location:** `app.py` - `/upload` endpoint (lines ~1372-1420)
- **Changes:**
  - ❌ Commented out file write operations to `uploads/images/` directory
  - ✅ Now saves images  ONLY to database `camera_image` BLOB column
  - Returns database image ID instead of file URL
  
**Before:**
```python
# Save to BOTH filesystem AND database
filepath = os.path.join(uploads_dir, filename)
with open(filepath, "wb") as f:
    f.write(image_data)
local_url = f'http://192.168.1.6:5000/uploads/images/{filename}'
```

**After:**
```python
# Save to database ONLY
cursor.execute('''
    INSERT INTO sensor_readings (device_id, camera_image, image_filename, timestamp)
    VALUES (?, ?, ?, CURRENT_TIMESTAMP)
''', ('ESP32_CAM', image_data, filename))
image_id = cursor.lastrowid
db_url = f'/api/image/{image_id}'
```

### 2. **Disabled Image Cleanup Task**
- **Location:** `app.py` (lines ~989-1035)
- **Changes:**
  - ❌ Commented out `cleanup_old_images()` function (no files to clean)
  - ❌ Commented out cleanup thread start
  - No more 30-second file deletion loops

### 3. **Updated Image Retrieval Endpoint**
- **Location:** `app.py` - `/api/latest-image` endpoint (lines ~1485-1530)
- **Changes:**
  - Now queries `camera_image` BLOB from database
  - Converts binary data to base64
  - Returns data URI: `data:image/jpeg;base64,{base64_data}`
  
**Before:**
```python
# Return file URL
image_url = f'http://192.168.1.6:5000/uploads/images/{filename}'
return jsonify({'image_url': image_url})
```

**After:**
```python
# Return base64 data URI from database
image_base64 = base64.b64encode(image_binary).decode('utf-8')
image_url = f'data:image/jpeg;base64,{image_base64}'
return jsonify({'image_url': image_url, 'source': 'database'})
```

### 4. **Updated WebSocket Camera Updates**
- **Location:** `app.py` - `broadcast_camera_update()` function (lines ~365-405)
- **Changes:**
  - Now accepts `image_id` parameter instead of file URL
  - Fetches image from database using ID
  - Converts to base64 and broadcasts data URI to clients
  
**Frontend Compatibility:** No changes needed! Frontend already accepts data URIs.

### 5. **Disabled Audio Upload Endpoint**
- **Location:** `app.py` - `/upload-audio` endpoint (lines ~1355-1390)
- **Changes:**
  - ❌ Commented out full audio handling logic
  - ✅ Replaced with placeholder endpoint returning `{'status': 'disabled'}`
  - **Reason:** ESP32 is not sending audio data (microphone disabled in firmware)

### 6. **Disabled AI Analysis Background Task**
- **Location:** `app.py` - `analyze_and_store_image()` function (lines ~315-360)
- **Changes:**
  - ❌ Commented out entire function (was file-path based)
  - **Reason:** Not compatible with database-only storage, and not currently triggered

## 📊 Database Schema (No Changes Required)

The existing database schema already supports BLOB storage:

```sql
CREATE TABLE sensor_readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    accel_x REAL,
    accel_y REAL,
    accel_z REAL,
    gyro_x REAL,
    gyro_y REAL,
    gyro_z REAL,
    mic_level REAL,
    sound_data TEXT,
    camera_image BLOB,        -- ✅ Stores binary image data
    image_filename TEXT,      -- ✅ Virtual filename for reference
    ai_caption TEXT,          -- ✅ AI-generated caption (optional)
    audio_data BLOB          -- ✅ Audio data (currently unused)
);
```

## 🎨 Frontend Compatibility

**No frontend changes required!** The frontend already supports both:
- File URLs: `http://192.168.1.6:5000/uploads/images/esp32_123456.jpg`
- Data URIs: `data:image/jpeg;base64,/9j/4AAQSkZJRg...`

Both are set via:
```javascript
document.getElementById('cameraImage').src = data.image_url;
```

## 📈 Benefits

### ✅ Advantages
1. **Simpler Architecture:** Single source of truth (database)
2. **No File Management:** No cleanup tasks, no filesystem I/O
3. **Cloud-Friendly:** Works on Cloud Run without persistent filesystem
4. **Atomic Operations:** Image and metadata saved together in one transaction
5. **Better Portability:** Database file contains everything

### ⚠️ Considerations
1. **Database Size:** BLOB data increases database file size
   - Mitigation: Implement database cleanup for old images (future)
2. **Memory Usage:** Base64 encoding increases data size by ~33%
   - Mitigation: Use `/api/image/<id>` endpoint for direct BLOB serving (already exists)

## 🔄 API Endpoint Changes

| Endpoint | Old Behavior | New Behavior |
|----------|--------------|--------------|
| `POST /upload` | Save to file + DB, return file URL | Save to DB only, return `/api/image/{id}` |
| `GET /api/latest-image` | Return file URL | Return base64 data URI |
| `GET /api/image/<id>` | *Already existed* | Serves BLOB directly (unchanged) |
| `POST /upload-audio` | Save audio to DB | Returns `{'status': 'disabled'}` |
| WebSocket `camera_update` | Broadcast file URL | Broadcast base64 data URI |

## 🚀 Deployment Notes

### Files to Deploy
- ✅ `app.py` (updated)
- ✅ `index.html` (no changes needed)
- ✅ `requirements.txt` (no changes)

### Files Now Obsolete
- ❌ `uploads/images/` directory (can be deleted)
- ❌ Image cleanup cron jobs (no longer needed)

### Environment Variables
No new environment variables required. Existing `.env` configuration works as-is.

## 🧪 Testing Checklist

- [ ] ESP32 uploads image → Check database for `camera_image` BLOB
- [ ] Frontend loads latest image → Verify base64 data URI in browser
- [ ] WebSocket updates camera → Confirm real-time image display
- [ ] No files created in `uploads/images/` directory
- [ ] `/upload-audio` returns `disabled` status (no crashes)
- [ ] Database size grows with each image upload

## 📝 Future Enhancements

1. **Database Cleanup:** Implement retention policy (keep last N images)
2. **Image Compression:** Compress BLOB data before storage
3. **Lazy Loading:** Load thumbnail first, full image on demand
4. **AI Analysis:** Re-enable AI captions with database-based workflow
5. **External Storage:** Migrate to cloud object storage (S3, GCS) if needed
