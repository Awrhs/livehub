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

    @classmethod
    def validate(cls):
        if cls.ENABLE_AUTH and cls.JWT_SECRET == DEFAULT_JWT_SECRET:
            raise RuntimeError('JWT_SECRET must be changed when ENABLE_AUTH=true')

    @classmethod
    def load_transcode_profiles(cls, config_path: str) -> Dict[str, Any]:
        with open(config_path, 'r', encoding='utf-8') as f:
            return json.load(f)
