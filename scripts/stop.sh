#!/bin/bash

echo "Stopping IPTV Streaming Media Cluster..."

# Stop SRS
if [ -f ./logs/srs.pid ]; then
    kill -TERM $(cat ./logs/srs.pid) 2>/dev/null || true
    rm ./logs/srs.pid
    echo "SRS stopped"
fi

# Stop Nginx
sudo nginx -s stop
echo "Nginx stopped"

# Stop gunicorn
pkill -f gunicorn.*src.app
echo "Backend API stopped"

echo "All services stopped"
