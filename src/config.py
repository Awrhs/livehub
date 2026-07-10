import os
import json
from typing import Dict, Any

DEFAULT_JWT_SECRET = 'your-secret-key-change-in-production'


class Config:
    SRS_HOST = os.getenv('SRS_HOST', '127.0.0.1')
    SRS_RTMP_PORT = int(os.getenv('SRS_RTMP_PORT', '1935'))
    SRS_HTTP_PORT = int(os.getenv('SRS_HTTP_PORT', '1985'))
    NGINX_HOST = os.getenv('NGINX_HOST', '127.0.0.1')
    NGINX_HTTP_PORT = int(os.getenv('NGINX_HTTP_PORT', '8080'))

    HLS_BASE_PATH = os.getenv('HLS_BASE_PATH', '/mnt/hls')
    VOD_BASE_PATH = os.getenv('VOD_BASE_PATH', '/mnt/vod')
    TIMESHIFT_BASE_PATH = os.getenv('TIMESHIFT_BASE_PATH', '/mnt/timeshift')

    HLS_FRAGMENT_DURATION = int(os.getenv('HLS_FRAGMENT_DURATION', '2'))
    HLS_WINDOW_SIZE = int(os.getenv('HLS_WINDOW_SIZE', '10'))

    ENABLE_TRANSCODE = os.getenv('ENABLE_TRANSCODE', 'true').lower() == 'true'
    FFMPEG_PATH = os.getenv('FFMPEG_PATH', 'ffmpeg')

    REDIS_URL = os.getenv('REDIS_URL', 'redis://127.0.0.1:6379/0')

    ENABLE_AUTH = os.getenv('ENABLE_AUTH', 'true').lower() == 'true'
    JWT_SECRET = os.getenv('JWT_SECRET', DEFAULT_JWT_SECRET)
    ALLOWED_ORIGINS = [
        origin.strip() for origin in os.getenv('ALLOWED_ORIGINS', '').split(',')
        if origin.strip()
    ]

    CLUSTER_NODES = [
        node.strip() for node in os.getenv('CLUSTER_NODES', '').split(',')
        if node.strip()
    ] or ['127.0.0.1']

    # Health check interval in seconds for load balancer
    HEALTH_CHECK_INTERVAL = int(os.getenv('HEALTH_CHECK_INTERVAL', '30'))

    # Load balancer health check port
    HEALTH_CHECK_PORT = int(os.getenv('HEALTH_CHECK_PORT', '8000'))

    # Multicast threshold (number of viewers before switching to multicast)
    MULTICAST_THRESHOLD = int(os.getenv('MULTICAST_THRESHOLD', '10'))

    # Bandwidth history settings for adaptive bitrate
    BANDWIDTH_HISTORY_LENGTH = int(os.getenv('BANDWIDTH_HISTORY_LENGTH', '10'))
    BANDWIDTH_HISTORY_TTL = int(os.getenv('BANDWIDTH_HISTORY_TTL', '3600'))

    # TS segment validation - minimum size in bytes (188 = 1 TS packet)
    TS_MIN_VALID_SIZE = int(os.getenv('TS_MIN_VALID_SIZE', '188'))

    # HLS settings
    HLS_VERSION = int(os.getenv('HLS_VERSION', '3'))

    # FFmpeg transcoding settings
    FFMPEG_PRESET = os.getenv('FFMPEG_PRESET', 'fast')
    MAX_CONCURRENT_TRANSCODES = int(os.getenv('MAX_CONCURRENT_TRANSCODES', '4'))

    # Cache sizes (MB)
    TS_CACHE_SIZE_MB = int(os.getenv('TS_CACHE_SIZE_MB', '500'))
    TIMESHIFT_CACHE_SIZE_MB = int(os.getenv('TIMESHIFT_CACHE_SIZE_MB', '2000'))

    # Adaptive bitrate thresholds (bps)
    ABR_THRESHOLD_1080P = int(os.getenv('ABR_THRESHOLD_1080P', '4500000'))  # 4.5 Mbps
    ABR_THRESHOLD_720P = int(os.getenv('ABR_THRESHOLD_720P', '2000000'))    # 2 Mbps
    ABR_THRESHOLD_480P = int(os.getenv('ABR_THRESHOLD_480P', '800000'))     # 800 kbps

    @classmethod
    def validate(cls):
        if cls.ENABLE_AUTH and cls.JWT_SECRET == DEFAULT_JWT_SECRET:
            raise RuntimeError('JWT_SECRET must be changed when ENABLE_AUTH=true')

    @classmethod
    def load_transcode_profiles(cls, config_path: str) -> Dict[str, Any]:
        with open(config_path, 'r', encoding='utf-8') as f:
            return json.load(f)
