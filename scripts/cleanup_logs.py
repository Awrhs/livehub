#!/usr/bin/env python3
import os
import logging
from datetime import datetime, timedelta

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


def cleanup_old_logs(log_dir: str, max_days: int = 7):
    if not os.path.exists(log_dir):
        logger.info(f"Log directory {log_dir} does not exist")
        return

    cutoff = datetime.now() - timedelta(days=max_days)
    cleaned = 0

    for filename in os.listdir(log_dir):
        file_path = os.path.join(log_dir, filename)
        if not os.path.isfile(file_path):
            continue

        mtime = datetime.fromtimestamp(os.path.getmtime(file_path))
        if mtime < cutoff:
            try:
                os.unlink(file_path)
                cleaned += 1
            except Exception as e:
                logger.warning(f"Failed to delete {file_path}: {e}")

    logger.info(f"Cleaned up {cleaned} old log files older than {max_days} days")


def cleanup_old_ts(hls_dir: str, max_age_hours: int = 1):
    if not os.path.exists(hls_dir):
        return

    cutoff = datetime.now() - timedelta(hours=max_age_hours)
    cleaned = 0

    for root, _, files in os.walk(hls_dir):
        for file in files:
            if file.endswith('.ts'):
                file_path = os.path.join(root, file)
                mtime = datetime.fromtimestamp(os.path.getmtime(file_path))
                if mtime < cutoff:
                    try:
                        os.unlink(file_path)
                        cleaned += 1
                    except Exception as e:
                        logger.warning(f"Failed to delete {file_path}: {e}")

    logger.info(f"Cleaned up {cleaned} old TS segments older than {max_age_hours} hour")


if __name__ == '__main__':
    import sys
    log_dir = './logs'
    hls_dir = '/mnt/hls'

    if len(sys.argv) >= 2:
        max_days = int(sys.argv[1])
    else:
        max_days = 7

    cleanup_old_logs(log_dir, max_days)
    cleanup_old_ts(hls_dir)
