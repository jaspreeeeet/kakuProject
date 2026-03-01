# 🖥️ KakuProject — New Laptop Setup Guide

Complete guide to clone, set up, and deploy this project on a fresh laptop.

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

## 🔧 Arduino IDE Setup (for ESP32 sketches)

### Board Manager
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

## 🚀 Step-by-Step: Clone & Run

### Step 1: Clone the Repo
```bash
git clone https://github.com/jaspreeeeet/kakuProject.git
cd kakuProject
```

### Step 2: Install Python deps
```bash
pip install -r requirements.txt
```

### Step 3: Run backend locally (optional test)
```bash
python app.py
```
Opens on http://localhost:8080

### Step 4: Deploy Backend to Cloud Run
```bash
gcloud auth login
gcloud config set project YOUR_PROJECT_ID

gcloud run deploy kakuproject \
  --source . \
  --region asia-south1 \
  --platform managed \
  --allow-unauthenticated \
  --memory 512Mi \
  --timeout 300
```

### Step 5: Deploy Frontend to Vercel
```bash
vercel login
vercel --prod
```

---

## 🔑 GitHub — Push from Another Account

If the new laptop has a **different GitHub account** logged in, here's how to push to this repo:

### Option A: Add as Collaborator (Recommended)
1. Go to https://github.com/jaspreeeeet/kakuProject/settings/access
2. Click **"Add people"**
3. Add the other GitHub username
4. The other account accepts the invite from their email/GitHub notifications
5. Now they can push directly:
   ```bash
   git clone https://github.com/jaspreeeeet/kakuProject.git
   cd kakuProject
   git push origin main
   ```

### Option B: Use a Personal Access Token (PAT)
If you want to push from the **same account** (jaspreeeeet) on the new laptop:

1. Go to https://github.com/settings/tokens → **"Generate new token (classic)"**
2. Give it a name like `new-laptop`
3. Select scopes: `repo` (full control)
4. Copy the token (starts with `ghp_...`)
5. On the new laptop, clone using the token:
   ```bash
   git clone https://jaspreeeeet:ghp_YOUR_TOKEN@github.com/jaspreeeeet/kakuProject.git
   ```
   Or update an existing clone:
   ```bash
   git remote set-url origin https://jaspreeeeet:ghp_YOUR_TOKEN@github.com/jaspreeeeet/kakuProject.git
   ```
6. Now `git push` works without login prompts.

### Option C: Switch Git Credentials on the Laptop
If the laptop has another account cached:
```bash
# Clear stored credentials (Windows)
# Open: Control Panel → Credential Manager → Windows Credentials
# Find "git:https://github.com" entries → Remove them

# Or from command line:
cmdkey /delete:git:https://github.com

# Next time you push, it will ask for username/password
# Enter: jaspreeeeet as username, and your PAT as password
git push origin main
```

### Option D: Use SSH Key (Most Secure)
1. Generate SSH key on the new laptop:
   ```bash
   ssh-keygen -t ed25519 -C "your-email@example.com"
   ```
2. Copy the public key:
   ```bash
   cat ~/.ssh/id_ed25519.pub
   ```
3. Add it to GitHub: https://github.com/settings/ssh/new
4. Switch remote to SSH:
   ```bash
   git remote set-url origin git@github.com:jaspreeeeet/kakuProject.git
   ```
5. Push:
   ```bash
   git push origin main
   ```

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
