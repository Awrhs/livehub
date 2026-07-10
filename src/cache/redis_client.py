"""
Redis client wrapper for caching, rate limiting, and JWT blacklisting.

Provides:
- Connection pooling
- Rate limiting with fixed window
- JWT token blacklisting
- Generic get/set operations
"""
import time
import logging
import json
from typing import Optional, Any

try:
    import redis
except ImportError:
    redis = None

logger = logging.getLogger(__name__)


class RedisClient:
    """
    Redis client wrapper with common operations for streaming server.

    Falls back to in-memory cache if Redis is not available.
    """

    def __init__(self, url: str = None, enable_fallback: bool = True):
        self.url = url
        self.enable_fallback = enable_fallback
        self._redis: Optional['redis.Redis'] = None
        self._fallback_cache: dict = {}  # Simple in-memory fallback
        self._fallback_expires: dict = {}
        self._connected = False
        self._connect()

    def _connect(self):
        """Connect to Redis"""
        if redis is None:
            logger.warning("Redis library not installed, using in-memory fallback")
            return

        if not self.url:
            logger.warning("Redis URL not configured, using in-memory fallback")
            return

        try:
            self._redis = redis.from_url(self.url, socket_timeout=5, socket_connect_timeout=5)
            # Test connection
            self._redis.ping()
            self._connected = True
            logger.info("Successfully connected to Redis")
        except Exception as e:
            logger.warning(f"Failed to connect to Redis: {e}, using in-memory fallback")

    def is_connected(self) -> bool:
        """Check if Redis is connected"""
        return self._connected

    def get(self, key: str) -> Optional[str]:
        """Get a value from Redis or fallback"""
        self._cleanup_fallback()

        if self._connected:
            try:
                return self._redis.get(key)
            except Exception as e:
                logger.debug(f"Redis get error: {e}")

        return self._fallback_cache.get(key)

    def setex(self, key: str, seconds: int, value: str) -> bool:
        """Set a value with expiration"""
        if self._connected:
            try:
                return self._redis.setex(key, seconds, value)
            except Exception as e:
                logger.debug(f"Redis setex error: {e}")

        # Fallback to in-memory
        self._fallback_cache[key] = value
        self._fallback_expires[key] = time.time() + seconds
        return True

    def delete(self, key: str) -> bool:
        """Delete a key"""
        if self._connected:
            try:
                return self._redis.delete(key) > 0
            except Exception as e:
                logger.debug(f"Redis delete error: {e}")

        if key in self._fallback_cache:
            del self._fallback_cache[key]
            if key in self._fallback_expires:
                del self._fallback_expires[key]
            return True
        return False

    def exists(self, key: str) -> bool:
        """Check if key exists"""
        self._cleanup_fallback()

        if self._connected:
            try:
                return self._redis.exists(key) > 0
            except Exception as e:
                logger.debug(f"Redis exists error: {e}")

        return key in self._fallback_cache

    def incr(self, key: str) -> int:
        """Increment a counter"""
        if self._connected:
            try:
                return self._redis.incr(key)
            except Exception as e:
                logger.debug(f"Redis incr error: {e}")

        # Fallback increment
        val = self._fallback_cache.get(key, 0)
        new_val = int(val) + 1 if val else 1
        self._fallback_cache[key] = str(new_val)
        return new_val

    def rate_limit(self, key: str, limit: int, window_seconds: int = 60) -> bool:
        """
        Check if rate limit has been exceeded.

        Args:
            key: Rate limit key (e.g., IP address)
            limit: Maximum requests per window
            window_seconds: Window size in seconds

        Returns:
            True if limit exceeded, False otherwise
        """
        now = int(time.time())
        window_key = f"rate:{key}:{now // window_seconds}"

        if self._connected:
            try:
                pipe = self._redis.pipeline()
                pipe.incr(window_key)
                pipe.expire(window_key, window_seconds)
                count, _ = pipe.execute()
                return count > limit
            except Exception as e:
                logger.debug(f"Redis rate_limit error: {e}")

        # Fallback implementation
        count = self.incr(window_key)
        if count == 1:  # First request, set expiration
            self._fallback_expires[window_key] = now + window_seconds

        return count > limit

    # JWT Blacklist operations

    def blacklist_token(self, token: str, expires_at: float) -> bool:
        """Add a token to blacklist"""
        ttl = max(1, int(expires_at - time.time()))
        return self.setex(f"blacklist:{token}", ttl, "1")

    def is_token_blacklisted(self, token: str) -> bool:
        """Check if token is blacklisted"""
        return self.exists(f"blacklist:{token}")

    # Cache operations

    def cache_get(self, key: str) -> Optional[Any]:
        """Get cached value with JSON deserialization"""
        value = self.get(f"cache:{key}")
        if value:
            try:
                return json.loads(value)
            except json.JSONDecodeError:
                return value
        return None

    def cache_set(self, key: str, value: Any, ttl_seconds: int = 300) -> bool:
        """Set cached value with JSON serialization"""
        if not isinstance(value, str):
            value = json.dumps(value)
        return self.setex(f"cache:{key}", ttl_seconds, value)

    def _cleanup_fallback(self):
        """Clean expired entries from fallback cache"""
        now = time.time()
        expired = [k for k, exp in self._fallback_expires.items() if exp < now]
        for k in expired:
            if k in self._fallback_cache:
                del self._fallback_cache[k]
            del self._fallback_expires[k]


# Global singleton instance
_redis_client: Optional[RedisClient] = None


def get_redis_client(url: str = None) -> RedisClient:
    """Get or create the global Redis client"""
    global _redis_client
    if _redis_client is None:
        from src.config import Config
        url = url or Config.REDIS_URL
        _redis_client = RedisClient(url=url)
    return _redis_client
