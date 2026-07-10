"""
Memory cache for HLS TS segments and other media content.

Provides LRU (Least Recently Used) eviction with size limits.
"""
import threading
import time
import os
import logging
from collections import OrderedDict
from typing import Optional, Dict, Tuple


logger = logging.getLogger(__name__)


class MediaLRUCache:
    """
    Thread-safe LRU memory cache for media content (TS files, etc.)

    Features:
    - Size-limited cache (MB)
    - LRU eviction when full
    - Thread-safe operations
    - Hit/miss statistics
    - Per-entry TTL support
    - Prometheus-compatible metrics
    """

    def __init__(self, max_size_mb: int = 500, default_ttl_seconds: int = 3600):
        self.max_size = max_size_mb * 1024 * 1024  # Convert to bytes
        self.current_size = 0
        self.default_ttl = default_ttl_seconds

        # Cache storage: path -> (content, timestamp, size)
        self._cache: OrderedDict[str, Tuple[bytes, float, int]] = OrderedDict()
        self._lock = threading.Lock()

        # Statistics
        self._hits = 0
        self._misses = 0
        self._evictions = 0

        logger.info(f"Media LRU cache initialized with {max_size_mb}MB limit")

    def get(self, path: str) -> Optional[bytes]:
        """Get content from cache. Returns None if not found or expired."""
        with self._lock:
            if path not in self._cache:
                self._misses += 1
                return None

            content, timestamp, size = self._cache[path]

            # Check TTL
            if time.time() - timestamp > self.default_ttl:
                del self._cache[path]
                self.current_size -= size
                self._misses += 1
                logger.debug(f"Cache entry expired: {path}")
                return None

            # Move to end (most recently used)
            self._cache.move_to_end(path)
            self._hits += 1

            return content

    def put(self, path: str, content: bytes, ttl_seconds: Optional[int] = None) -> bool:
        """
        Add content to cache.

        Returns True if successfully added, False if content too large.
        """
        content_size = len(content)

        # Don't cache files larger than 50% of max size
        if content_size > self.max_size // 2:
            logger.debug(f"Not caching {path}: size {content_size/1024/1024:.2f}MB too large")
            return False

        with self._lock:
            # Remove existing entry if present
            if path in self._cache:
                _, _, old_size = self._cache[path]
                self.current_size -= old_size
                del self._cache[path]

            # Evict oldest entries until we have space
            while self.current_size + content_size > self.max_size:
                if not self._cache:
                    break  # Shouldn't happen due to 50% check above

                evicted_path, (_, _, evicted_size) = self._cache.popitem(last=False)
                self.current_size -= evicted_size
                self._evictions += 1
                logger.debug(f"Evicted from cache: {evicted_path}")

            # Add new entry
            timestamp = time.time()
            self._cache[path] = (content, timestamp, content_size)
            self.current_size += content_size

            logger.debug(f"Cached {path}: {content_size/1024:.2f}KB")
            return True

    def remove(self, path: str) -> bool:
        """Remove a specific entry from cache"""
        with self._lock:
            if path in self._cache:
                _, _, size = self._cache[path]
                del self._cache[path]
                self.current_size -= size
                return True
            return False

    def clear(self):
        """Clear all entries from cache"""
        with self._lock:
            self._cache.clear()
            self.current_size = 0
            logger.info("Cache cleared")

    def get_stats(self) -> Dict:
        """Get cache statistics"""
        with self._lock:
            total = self._hits + self._misses
            hit_rate = self._hits / total if total > 0 else 0.0
            return {
                'hits': self._hits,
                'misses': self._misses,
                'hit_rate': hit_rate,
                'evictions': self._evictions,
                'size_bytes': self.current_size,
                'size_mb': self.current_size / 1024 / 1024,
                'max_size_mb': self.max_size / 1024 / 1024,
                'entries': len(self._cache),
            }

    def get_hit_rate(self) -> float:
        """Get current hit rate"""
        total = self._hits + self._misses
        return self._hits / total if total > 0 else 0.0


class FileSystemCache:
    """
    Hybrid cache that stores large files on disk and small files in memory.

    Useful for timeshift concat results and other large generated files.
    """

    def __init__(self, cache_dir: str, max_size_mb: int = 2000,
                 memory_cache_threshold_kb: int = 500):
        self.cache_dir = cache_dir
        self.max_size = max_size_mb * 1024 * 1024
        self.memory_threshold = memory_cache_threshold_kb * 1024

        # Memory cache for small files
        self._memory_cache = MediaLRUCache(max_size_mb=max_size_mb // 4)

        # Disk cache tracking
        self._disk_entries: OrderedDict[str, Tuple[str, int, float]] = OrderedDict()
        self._disk_size = 0
        self._lock = threading.Lock()

        os.makedirs(cache_dir, exist_ok=True)
        logger.info(f"FileSystem cache initialized at {cache_dir} with {max_size_mb}MB limit")

    def get(self, key: str) -> Optional[bytes]:
        """Get content from cache"""
        # Try memory cache first
        content = self._memory_cache.get(key)
        if content is not None:
            return content

        # Try disk cache
        with self._lock:
            if key not in self._disk_entries:
                return None

            file_path, size, _ = self._disk_entries[key]

        try:
            with open(file_path, 'rb') as f:
                content = f.read()

            # If small enough, also cache in memory
            if len(content) <= self.memory_threshold:
                self._memory_cache.put(key, content)

            return content
        except (IOError, OSError) as e:
            logger.debug(f"Error reading cached file {file_path}: {e}")
            with self._lock:
                if key in self._disk_entries:
                    del self._disk_entries[key]
            return None

    def put(self, key: str, content: bytes) -> bool:
        """Put content in cache"""
        content_size = len(content)

        # Always cache small files in memory
        if content_size <= self.memory_threshold:
            self._memory_cache.put(key, content)

        # Cache everything to disk if within reasonable limits
        if content_size > self.max_size // 4:
            logger.debug(f"Content too large for disk cache: {content_size/1024/1024:.2f}MB")
            return False

        with self._lock:
            # Evict oldest until we have space
            while self._disk_size + content_size > self.max_size:
                if not self._disk_entries:
                    break

                evicted_key, (evicted_path, evicted_size, _) = self._disk_entries.popitem(last=False)
                try:
                    os.unlink(evicted_path)
                except OSError:
                    pass
                self._disk_size -= evicted_size
                logger.debug(f"Evicted from disk cache: {evicted_path}")

            # Write to disk
            file_path = os.path.join(self.cache_dir, f"{hash(key) & 0xFFFFFFFF:08x}.cache")
            try:
                with open(file_path, 'wb') as f:
                    f.write(content)

                self._disk_entries[key] = (file_path, content_size, time.time())
                self._disk_size += content_size
                return True
            except IOError as e:
                logger.error(f"Error writing to cache {file_path}: {e}")
                return False

    def get_stats(self) -> Dict:
        """Get combined cache statistics"""
        memory_stats = self._memory_cache.get_stats()
        with self._lock:
            return {
                'memory': memory_stats,
                'disk': {
                    'size_bytes': self._disk_size,
                    'size_mb': self._disk_size / 1024 / 1024,
                    'entries': len(self._disk_entries),
                }
            }


# Global singleton instances
_ts_cache: Optional[MediaLRUCache] = None
_timeshift_cache: Optional[FileSystemCache] = None


def get_ts_cache(max_size_mb: int = 500) -> MediaLRUCache:
    """Get or create the global TS segment memory cache"""
    global _ts_cache
    if _ts_cache is None:
        _ts_cache = MediaLRUCache(max_size_mb=max_size_mb)
    return _ts_cache


def get_timeshift_cache(cache_dir: str = None, max_size_mb: int = 2000) -> FileSystemCache:
    """Get or create the global timeshift cache"""
    global _timeshift_cache
    if _timeshift_cache is None:
        from src.config import Config
        cache_dir = cache_dir or os.path.join(Config.TIMESHIFT_BASE_PATH, '.cache')
        _timeshift_cache = FileSystemCache(cache_dir, max_size_mb=max_size_mb)
    return _timeshift_cache
