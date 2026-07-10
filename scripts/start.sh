#!/bin/bash

echo "Starting IPTV Streaming Media Cluster..."

# Apply sysctl optimizations
if [ -f ./config/sysctl/streaming.conf ]; then
    echo "Applying kernel network optimizations..."
    sudo sysctl -p ./config/sysctl/streaming.conf
fi

# Increase open file limits
ulimit -n 65535

# Start SRS
echo "Starting SRS server..."
cd ./srs && ./objs/srs -c ../config/srs.conf &
SRS_PID=$!
echo "SRS started with PID $SRS_PID"

# Start Nginx-RTMP
echo "Starting Nginx-RTMP server..."
sudo nginx -c ./config/nginx.conf
echo "Nginx started"

# Activate virtual environment if exists
if [ -d ./venv ]; then
    echo "Activating Python virtual environment..."
    source ./venv/bin/activate
fi

# Install dependencies if needed
pip install -q -r requirements.txt

# Start backend API with gunicorn
echo "Starting backend API server..."
cd ..
gunicorn -c gunicorn_config.py src.app:init_app()

echo "All services started"
