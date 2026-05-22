import os
import re
from typing import Iterable

from werkzeug.utils import safe_join

SAFE_NAME_RE = re.compile(r'^[A-Za-z0-9_.-]+$')


def validate_safe_name(value: str, field_name: str) -> str:
    if not value or not SAFE_NAME_RE.fullmatch(value):
        raise ValueError(f'Invalid {field_name}')
    return value


def safe_media_path(base_path: str, *parts: Iterable[str]) -> str:
    path = safe_join(base_path, *parts)
    if path is None:
        raise ValueError('Invalid media path')
    return path
