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
gunicorn -w 4 -b 0.0.0.0:8000 -t 120 --worker-class gevent src.app:init_app() --access-logfile logs/api_access.log --error-logfile logs/api_error.log

echo "All services started"
