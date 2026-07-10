"""
Rate limiting middleware for Flask routes.

Provides per-IP and per-user rate limiting using Redis.
"""
import logging
from functools import wraps
from flask import request, jsonify, g

from src.cache.redis_client import get_redis_client

logger = logging.getLogger(__name__)


class RateLimiter:
    """Rate limiter decorator factory"""

    def __init__(self, limit: int = 100, window: int = 60, by_ip: bool = True,
                 limit_id: str = "default"):
        """
        Create a rate limiter decorator.

        Args:
            limit: Maximum requests per window
            window: Window size in seconds
            by_ip: Whether to limit by IP (True) or by user ID (False)
            limit_id: Identifier for this specific limit
        """
        self.limit = limit
        self.window = window
        self.by_ip = by_ip
        self.limit_id = limit_id
        self._redis = get_redis_client()

    def __call__(self, f):
        @wraps(f)
        def wrapped(*args, **kwargs):
            # Determine limit key
            if self.by_ip:
                key = f"{self.limit_id}:{request.remote_addr}"
            else:
                # Use authenticated user ID if available
                user_id = getattr(g, 'user_id', request.remote_addr)
                key = f"{self.limit_id}:{user_id}"

            # Check rate limit
            if self._redis.rate_limit(key, self.limit, self.window):
                logger.warning(f"Rate limit exceeded for {key}")
                return jsonify({
                    'error': 'Rate limit exceeded',
                    'limit': self.limit,
                    'window_seconds': self.window
                }), 429

            return f(*args, **kwargs)

        return wrapped


# Pre-configured rate limiters for common use cases

# Playback endpoints - more lenient (300 requests/min = 5/sec)
playback_rate_limit = RateLimiter(limit=300, window=60, by_ip=True, limit_id="playback")

# API endpoints - standard (100 requests/min)
api_rate_limit = RateLimiter(limit=100, window=60, by_ip=True, limit_id="api")

# Auth endpoints - strict (10 requests/min) - prevents brute force
auth_rate_limit = RateLimiter(limit=10, window=60, by_ip=True, limit_id="auth")

# Transcode endpoints - very strict (5 requests/min) - expensive operations
transcode_rate_limit = RateLimiter(limit=5, window=60, by_ip=True, limit_id="transcode")

# Admin endpoints - strict (20 requests/min)
admin_rate_limit = RateLimiter(limit=20, window=60, by_ip=True, limit_id="admin")


def limit(limit: int = 100, window: int = 60, limit_id: str = "custom"):
    """Custom rate limiter decorator"""
    return RateLimiter(limit=limit, window=window, limit_id=limit_id)
