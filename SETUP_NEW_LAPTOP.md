# 🖥️ KakuProject — New Laptop Setup Guide (From USB Pendrive)

**📌 PRIMARY USE CASE:** You have this entire folder on a USB pendrive and want to:
- Run it on a new laptop
- Push changes to GitHub (even if new laptop has different GitHub account)
- Deploy to Cloud Run / Vercel

**💾 This folder includes everything:**
- All code (app.py, test.ino, esp32_sketch, etc.)
- Database (sensor_data.db) with all your data
- Git history (.git folder)
- No need to clone from GitHub!

---

## ⚡ TL;DR - Just Want to Push to GitHub?

**On the new laptop (one-time setup):**

1. **Get a Personal Access Token:**
   - Go to https://github.com/settings/tokens
   - Click "Generate new token (classic)"
   - Check `repo`, generate, copy the token (starts with `ghp_...`)

2. **Navigate to your USB folder and run:**
   ```bash
   cd F:\kakuProject-main    # Or E:, G:, etc. depending on your USB drive
   git remote set-url origin https://jaspreeeeet:YOUR_TOKEN@github.com/jaspreeeeet/kakuProject.git
   ```

3. **Done! Now you can push anytime:**
   ```bash
   git add .
   git commit -m "your changes"
   git push origin main
   ```

**Continue reading for:** Python setup, Arduino IDE, deployment, etc.

---

## 📋 What You Need to Install

### 1. Git
- Download: https://git-scm.com/downloads
- During install, select **"Git from the command line and also from 3rd-party software"**

### 2. Python 3.10+
- Download: https://www.python.org/downloads/
- **⚠️ CHECK "Add Python to PATH"** during installation

### 3. Node.js (for Vercel CLI)
- Download: https://nodejs.org/ (LTS version)
- Includes npm automatically

### 4. Google Cloud CLI (gcloud)
- Download: https://cloud.google.com/sdk/docs/install
- Run the installer, it adds `gcloud` to PATH

### 5. Vercel CLI
```bash
npm install -g vercel
```

### 6. Arduino IDE 2.x (for ESP32 sketches)
- Download: https://www.arduino.cc/en/software
- Version used: **Arduino IDE 2.3.7**

---

## 📦 Python Dependencies

Install after cloning:
```bash
pip install -r requirements.txt
```

Contents of `requirements.txt`:
```
Flask==2.3.2
Flask-CORS==4.0.0
flask-socketio==5.3.4
python-socketio==5.9.0
python-engineio==4.7.1
Werkzeug==2.3.6
gunicorn==20.1.0
requests>=2.28.0
setuptools==79.0.1
```

---

## � Quick Start (From USB Pendrive)

### Step 1: Plug in USB and Navigate to Folder
```bash
# Check which drive letter your USB is (look in File Explorer)
# For example, if it's F drive:
cd F:\kakuProject-main

# Or E drive:
cd E:\kakuProject-main
```

### Step 2: Install Python Dependencies (first time only)
```bash
pip install -r requirements.txt
```

### Step 3: Configure Git to Push (first time only)

The folder already has Git set up, but the new laptop needs authentication.

**IMPORTANT:** Don't worry if the new laptop has a different GitHub account logged in. You have 3 simple options:

#### Option A: Use Personal Access Token (Easiest, Recommended)
1. On any device, go to: https://github.com/settings/tokens
2. Click **"Generate new token (classic)"**
3. Give it a name: `my-laptop` or `pendrive-setup`
4. Check the box for `repo` (full repository access)
5. Click **"Generate token"** at bottom
6. Copy the token (starts with `ghp_...`) — **Save it somewhere safe!**
7. On the new laptop, run this command (replace `YOUR_TOKEN` with the actual token):
   ```bash
   git remote set-url origin https://jaspreeeeet:YOUR_TOKEN@github.com/jaspreeeeet/kakuProject.git
   ```
8. Test it:
   ```bash
   git push origin main
   ```
   Should work without asking for password!

#### Option B: Clear Old Credentials (If laptop has wrong account)
```bash
# Windows: Remove old GitHub credentials
cmdkey /delete:git:https://github.com

# OR use GUI:
# Control Panel → Credential Manager → Windows Credentials
# Find "git:https://github.com" entries → Remove All

# Next git push will ask for username/password
git push origin main
# Username: jaspreeeeet
# Password: ghp_YOUR_TOKEN (from Option A)
```

#### Option C: SSH Key (Most Secure, Takes 2 Minutes)
```bash
# 1. Generate SSH key on new laptop
ssh-keygen -t ed25519 -C "your-email@example.com"
# Press Enter 3 times (default location, no passphrase)

# 2. Copy the public key
cat ~/.ssh/id_ed25519.pub
# Or on Windows: type C:\Users\YourName\.ssh\id_ed25519.pub

# 3. Add to GitHub
# Go to: https://github.com/settings/ssh/new
# Paste the key, give it a name like "my-laptop"

# 4. Change Git remote to use SSH
git remote set-url origin git@github.com:jaspreeeeet/kakuProject.git

# 5. Test
git push origin main
```

### Step 4: Now You Can Push Changes Anytime!
```bash
# Make changes to any file...
git add .
git commit -m "your message here"
git push origin main
```

### Step 5: Deploy (Optional, only when you want to update live site)
```bash
# Backend to Cloud Run
gcloud auth login
gcloud run deploy kakuproject --source . --region asia-south1 --allow-unauthenticated --memory 512Mi --timeout 300

# Frontend to Vercel
vercel login
vercel --prod
```

---

## 🔧 Arduino IDE Setup (for ESP32 sketches)
1. Open Arduino IDE → **File → Preferences**
2. In **"Additional Board Manager URLs"**, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Go to **Tools → Board → Board Manager**, search **"esp32"**, install **"esp32 by Espressif Systems"** (v2.x or v3.x)

### Board Selection
- **Board:** `XIAO_ESP32S3`
- **Port:** whichever COM port the device shows up on (e.g. COM3)
- **Upload Speed:** 921600
- **PSRAM:** `OPI PSRAM` (enabled)
- **Partition Scheme:** Default or "Huge APP (3MB No OTA)"

### Required Libraries (install via Library Manager)
- `ArduinoJson` by Benoit Blanchon (v6 or v7)
- `ESP32 Camera` (included with esp32 board package)
- `WiFi` / `HTTPClient` / `WiFiClientSecure` (included with esp32 board package)

### Sketches
| File | Purpose |
|------|---------|
| `test.ino` | BLIP/ViT image captioning test |
| `esp32_sketch/esp32_sketch.ino` | Main production pet firmware |

---

##  Important Notes When Running from USB Pendrive

### File Paths
- The pendrive drive letter may change on different laptops (E:, F:, G:, etc.)
- Always `cd` into the project folder before running commands
- For Arduino: Upload the `.ino` file from wherever the pendrive is mounted

### Database
- The SQLite database (`sensor_data.db`) travels with the folder
- All your sensor data, images, and AI captions are preserved
- First time on new laptop: database already exists, no setup needed

### Git
- The `.git` folder is included, so Git history is preserved
- You just need to authenticate (see GitHub section above)
- After first push, subsequent pushes work normally

### Potential Issues
| Issue | Solution |
|-------|----------|
| `pip install` fails | Use `python -m pip install -r requirements.txt` |
| Drive letter changed | Update `cd` command to new drive (check in File Explorer) |
| "Permission denied" on push | Clear Windows credentials or use PAT (see GitHub section) |
| Database locked | Close any running `python app.py` instances |
| Arduino sketch paths | Open `.ino` directly from pendrive location |

---

## 🌐 Accounts & Services Needed

| Service | URL | What For |
|---------|-----|----------|
| **GitHub** | github.com | Code hosting, version control |
| **Google Cloud** | console.cloud.google.com | Backend hosting (Cloud Run) |
| **Vercel** | vercel.com | Frontend hosting (static HTML) |
| **HuggingFace** | huggingface.co | AI image analysis API (Google ViT model) |

### API Keys / Tokens
| Token | Where to Get | Currently Set In |
|-------|-------------|------------------|
| **HF_TOKEN** | https://huggingface.co/settings/tokens | Hardcoded in `app.py` (lines 21-22) |
| **GCP Project** | `gcloud config set project PROJECT_ID` | CLI config |
| **Vercel** | `vercel login` | CLI config |

---

## 📂 Project Structure Overview

```
kakuProject/
├── app.py                 ← Flask backend (3600 lines) — THE MAIN SERVER
├── index.html             ← Frontend dashboard (Vercel)
├── requirements.txt       ← Python dependencies
├── Dockerfile             ← Cloud Run container config
├── vercel.json            ← Vercel routing config
├── test.ino               ← ESP32 image captioning test sketch
├── esp32_sketch/
│   ├── esp32_sketch.ino   ← Main ESP32 pet firmware
│   ├── all_pets.h         ← Pet animation sprites
│   └── qr_wifi_setup.h    ← QR code WiFi setup
├── api/
│   └── app.py             ← Vercel serverless function (if needed)
├── uploads/images/        ← Uploaded images directory
├── DEPLOY.md              ← Deployment commands
└── SETUP_NEW_LAPTOP.md    ← THIS FILE
```

---

## ⚡ Quick Reference Commands

```bash
# --- Git ---
git add .
git commit -m "message"
git push origin main

# --- Backend Deploy ---
gcloud run deploy kakuproject --source . --region asia-south1 --allow-unauthenticated --memory 512Mi --timeout 300

# --- Frontend Deploy ---
vercel --prod

# --- View Cloud Run Logs ---
gcloud run logs read kakuproject --region asia-south1 --limit 50

# --- Run Backend Locally ---
python app.py

# --- Check Cloud Run Status ---
gcloud run services describe kakuproject --region asia-south1 --format='value(status.url)'
```

---

## 🐛 Common Issues on New Laptop

| Problem | Fix |
|---------|-----|
| `python` not found | Add Python to PATH, or use `python3` |
| `gcloud` not found | Re-install Google Cloud CLI, restart terminal |
| `vercel` not found | Run `npm install -g vercel` |
| `git push` permission denied | See GitHub section above (Option A/B/C/D) |
| Arduino can't find board | Install esp32 board package in Board Manager |
| ESP32 COM port not showing | Install CP2102 or CH340 USB driver |
| `pip install` fails | Try `python -m pip install -r requirements.txt` |
| Cloud Run deploy fails | Check `gcloud auth login` and project ID |
