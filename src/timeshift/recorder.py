import os
import logging
import subprocess
import hashlib
from datetime import datetime, timedelta
from typing import Optional, List
from src.config import Config
from src.security import validate_input_url, validate_output_path, validate_safe_name
from src.streaming.cache import get_timeshift_cache

logger = logging.getLogger(__name__)

# Initialize timeshift result cache
timeshift_cache = get_timeshift_cache()


class TimeshiftRecorder:
    def __init__(self, storage_path: str = None):
        self.storage_path = storage_path or Config.TIMESHIFT_BASE_PATH
        os.makedirs(self.storage_path, exist_ok=True)
        self.active_records: dict = {}

    def start_recording(self, channel_id: str, input_url: str) -> bool:
        if channel_id in self.active_records:
            logger.warning(f"Already recording {channel_id}")
            return False

        # Validate inputs for security
        validate_safe_name(channel_id, "channel_id")
        validate_input_url(input_url)

        date_str = datetime.now().strftime('%Y%m%d')
        output_dir = os.path.join(self.storage_path, channel_id, date_str)
        validate_output_path(output_dir)
        os.makedirs(output_dir, exist_ok=True)

        output_file = os.path.join(output_dir, f"{datetime.now():%H%M%S}.ts")

        cmd = [
            'ffmpeg',
            '-i', input_url,
            '-c', 'copy',
            '-bsf:v', 'h264_mp4toannexb',
            '-y',
            output_file
        ]

        try:
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            self.active_records[channel_id] = {
                'process': process,
                'start_time': datetime.now(),
                'output_dir': output_dir,
                'current_file': output_file
            }
            logger.info(f"Started timeshift recording for {channel_id}")
            return True
        except Exception as e:
            logger.error(f"Failed to start recording {channel_id}: {e}")
            return False

    def stop_recording(self, channel_id: str) -> bool:
        if channel_id not in self.active_records:
            return False

        record = self.active_records[channel_id]
        process = record['process']

        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()

        del self.active_records[channel_id]
        logger.info(f"Stopped timeshift recording for {channel_id}")
        return True

    def get_record_segment(self, channel_id: str, start_time: datetime,
                          duration: int) -> Optional[bytes]:
        """
        Get timeshift segment as bytes. Uses cache for repeated requests.

        Returns bytes content instead of file path for cache efficiency.
        Returns None if segment not found.
        """
        validate_safe_name(channel_id, "channel_id")

        # Generate cache key from request parameters
        cache_key = hashlib.md5(
            f"{channel_id}:{start_time.isoformat()}:{duration}".encode()
        ).hexdigest()

        # Try cache first
        cached_content = timeshift_cache.get(cache_key)
        if cached_content is not None:
            logger.debug(f"Timeshift cache hit: {cache_key}")
            return cached_content

        date_str = start_time.strftime('%Y%m%d')
        channel_dir = os.path.join(self.storage_path, channel_id, date_str)
        validate_output_path(channel_dir)

        if not os.path.exists(channel_dir):
            return None

        target_start = start_time
        target_end = start_time + timedelta(seconds=duration)

        segments = []
        for filename in sorted(os.listdir(channel_dir)):
            if not filename.endswith('.ts'):
                continue

            try:
                file_hhmmss = filename.split('.')[0]
                file_hh = int(file_hhmmss[:2])
                file_mm = int(file_hhmmss[2:4])
                file_ss = int(file_hhmmss[4:])
                file_dt = start_time.replace(hour=file_hh, minute=file_mm, second=file_ss)

                file_size = os.path.getsize(os.path.join(channel_dir, filename))
                est_duration = file_size / (188 * 188 * 25)

                if (file_dt >= target_start and file_dt <= target_end):
                    segments.append(os.path.join(channel_dir, filename))
            except Exception as e:
                logger.warning(f"Failed to parse segment {filename}: {e}")
                continue

        if not segments:
            return None

        result_content = self._concat_segments(segments, channel_id, start_time, duration)

        # Cache successful result
        if result_content is not None:
            timeshift_cache.put(cache_key, result_content)
            logger.debug(f"Cached timeshift result: {cache_key} ({len(result_content)/1024/1024:.2f}MB)")

        return result_content

    def _concat_segments(self, segments: List[str], channel_id: str,
                        start_time: datetime, duration: int) -> Optional[bytes]:
        """Concat segments and return bytes content"""
        concat_file = os.path.join(
            self.storage_path,
            channel_id,
            f"concat_{start_time.timestamp()}.txt"
        )
        validate_output_path(concat_file)

        with open(concat_file, 'w') as f:
            for seg in segments:
                f.write(f"file '{seg}'\n")

        output_path = os.path.join(
            self.storage_path,
            channel_id,
            f"output_{start_time.timestamp()}.ts"
        )

        cmd = [
            'ffmpeg',
            '-f', 'concat',
            '-safe', '0',
            '-i', concat_file,
            '-c', 'copy',
            '-y',
            output_path
        ]

        try:
            result = subprocess.run(cmd, capture_output=True, timeout=300)
            os.unlink(concat_file)
            if result.returncode == 0:
                # Read result and cleanup
                with open(output_path, 'rb') as f:
                    content = f.read()
                os.unlink(output_path)
                return content
            else:
                logger.error(f"Concat failed: {result.stderr.decode()}")
                return None
        except Exception as e:
            logger.error(f"Failed to concat segments: {e}")
            return None

    def cleanup_old_records(self, max_days: int = 7):
        cutoff = datetime.now() - timedelta(days=max_days)
        cleaned = 0

        for channel_id in os.listdir(self.storage_path):
            channel_dir = os.path.join(self.storage_path, channel_id)
            if not os.path.isdir(channel_dir):
                continue

            for date_str in os.listdir(channel_dir):
                date_path = os.path.join(channel_dir, date_str)
                try:
                    date = datetime.strptime(date_str, '%Y%m%d')
                    if date < cutoff:
                        for file in os.listdir(date_path):
                            os.unlink(os.path.join(date_path, file))
                        os.rmdir(date_path)
                        cleaned += 1
                except Exception as e:
                    logger.warning(f"Failed to cleanup {date_path}: {e}")

        logger.info(f"Cleaned up {cleaned} old days of recording")
