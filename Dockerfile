FROM python:3.11-slim

LABEL maintainer="IPTV Streaming Cluster"

RUN apt-get update && apt-get install -y --no-install-recommends \
    ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

RUN mkdir -p logs /mnt/hls /mnt/vod /mnt/timeshift

EXPOSE 8000

CMD ["gunicorn", "-c", "gunicorn_config.py", "src.app:init_app()"]
