# Quick Deploy Commands

## Frontend (Vercel - Static HTML)
```bash
vercel --prod
```
**Live at:** https://kakuproject.vercel.app

---

## Backend (Google Cloud Run - Python Flask)

### Prerequisites
```bash
# Install Google Cloud CLI
# https://cloud.google.com/sdk/docs/install

# Login to Google Cloud
gcloud auth login

# Set your project
gcloud config set project YOUR_PROJECT_ID
```

### Deploy Backend (app.py)
```bash
# Navigate to project directory
cd f:/kakuProject-main

# Deploy to Cloud Run
gcloud run deploy kakuproject \
  --source . \
  --region asia-south1 \
  --platform managed \
  --allow-unauthenticated \
  --memory 512Mi \
  --timeout 300 \
  --max-instances 10

# OR use Docker build + deploy
docker build -t gcr.io/YOUR_PROJECT_ID/kakuproject .
docker push gcr.io/YOUR_PROJECT_ID/kakuproject
gcloud run deploy kakuproject \
  --image gcr.io/YOUR_PROJECT_ID/kakuproject \
  --region asia-south1 \
  --platform managed \
  --allow-unauthenticated
```

**Live at:** https://kakuproject-90943350924.asia-south1.run.app

---

## Quick Fix After Code Changes

**After editing app.py or any backend file:**
```bash
# 1. Commit to GitHub (optional)
git add .
git commit -m "Fix: description"
git push

# 2. Redeploy to Cloud Run (REQUIRED)
gcloud run deploy kakuproject \
  --source . \
  --region asia-south1 \
  --platform managed \
  --allow-unauthenticated
```

**After editing index.html (frontend):**
```bash
vercel --prod
```

---

## Check Deployment Status

```bash
# Check Cloud Run services
gcloud run services list

# View logs
gcloud run logs read kakuproject --region asia-south1 --limit 50

# Get service URL
gcloud run services describe kakuproject --region asia-south1 --format='value(status.url)'
```

---

## Troubleshooting

**Error: Server Error 500**
- Backend not redeployed after code changes
- Check logs: `gcloud run logs read kakuproject --region asia-south1`
- Verify syntax errors with: `python app.py` locally

**Error: CORS issues**
- Backend needs CORS headers enabled (already in app.py)
- Check `CORS(app)` is present in app.py

**Error: Timeout**
- Increase timeout: `--timeout 300` in deploy command
- Increase memory: `--memory 512Mi` or `--memory 1Gi`
