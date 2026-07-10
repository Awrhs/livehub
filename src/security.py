import logging
import re
from typing import Iterable

from werkzeug.utils import safe_join


logger = logging.getLogger(__name__)

SAFE_NAME_RE = re.compile(r'^[A-Za-z0-9_.-]+$')

# Allowed URL schemes for FFmpeg input
ALLOWED_URL_SCHEMES = {
    'rtmp://',
    'rtsp://',
    'http://',
    'https://',
    'file://',
    'srt://',
    'udp://',
    'tcp://'
}

# Characters that could lead to command injection
DANGEROUS_CHARS = {
    '`', '$', ';', '|', '&', '<', '>', '"', "'",
    '\\', '\n', '\r', '\t', '(', ')', '[', ']',
    '{', '}', '*', '?', '!', '#', '~', '='
}


def validate_safe_name(value: str, field_name: str = "name") -> str:
    """Validate that a name contains only safe characters"""
    if not value or not SAFE_NAME_RE.fullmatch(value):
        raise ValueError(f'Invalid {field_name}: {value}')
    return value


def safe_media_path(base_path: str, *parts: Iterable[str]) -> str:
    """Safely join path parts and validate no path traversal"""
    path = safe_join(base_path, *parts)
    if path is None:
        raise ValueError('Invalid media path')
    return path


def validate_input_url(url: str) -> str:
    """
    Validate FFmpeg input URL to prevent command injection attacks.

    Args:
        url: The input URL to validate

    Returns:
        The validated URL if safe

    Raises:
        ValueError: If the URL contains dangerous patterns
    """
    if not url:
        raise ValueError("URL cannot be empty")

    # Check for valid scheme
    url_lower = url.lower()
    if not any(url_lower.startswith(scheme) for scheme in ALLOWED_URL_SCHEMES):
        logger.error(f"URL has unsupported scheme: {url[:50]}...")
        raise ValueError(f"Unsupported URL scheme. Allowed: {', '.join(ALLOWED_URL_SCHEMES)}")

    # Check for dangerous characters
    found_dangerous = [c for c in url if c in DANGEROUS_CHARS]
    if found_dangerous:
        logger.error(f"URL contains dangerous characters: {found_dangerous} in {url[:50]}...")
        raise ValueError("URL contains dangerous characters that could lead to command injection")

    # Check for potential command substitution patterns
    if '$(' in url or '`' in url:
        logger.error(f"URL contains command substitution pattern: {url[:50]}...")
        raise ValueError("URL contains command substitution patterns")

    # Check for shell redirection
    if ' >' in url or ' 1>' in url or ' 2>' in url or ' &>' in url:
        logger.error(f"URL contains shell redirection: {url[:50]}...")
        raise ValueError("URL contains shell redirection patterns")

    return url


def validate_output_path(path: str) -> str:
    """
    Validate output path to prevent path traversal and ensure it's safe.

    Args:
        path: The output path to validate

    Returns:
        The validated path if safe

    Raises:
        ValueError: If the path is dangerous
    """
    if not path:
        raise ValueError("Path cannot be empty")

    # Check for path traversal
    if '..' in path.split('/') or '..' in path.split('\\'):
        logger.error(f"Path contains traversal attempt: {path}")
        raise ValueError("Path contains path traversal")

    # Check for dangerous characters
    found_dangerous = [c for c in path if c in DANGEROUS_CHARS]
    if found_dangerous:
        logger.error(f"Path contains dangerous characters: {found_dangerous} in {path}")
        raise ValueError("Path contains dangerous characters")

    return path
