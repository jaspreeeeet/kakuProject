# Use official Python slim image
FROM python:3.10-slim

# Set working directory
WORKDIR /app

# Copy project files
COPY . /app

# Install dependencies
RUN pip install --no-cache-dir -r requirements.txt

# Set environment variable for port
ENV PORT=8080

# Start the app with Gunicorn
# --timeout 300 prevents worker kill during large firmware downloads (~40s for 1MB)
CMD ["gunicorn", "--bind", ":8080", "--worker-class", "eventlet", "--workers", "1", "--timeout", "300", "app:app"]
