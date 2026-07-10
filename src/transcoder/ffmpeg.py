import subprocess
import logging
import os
import time
from typing import List, Dict, Optional
from src.streaming.profile_manager import TranscodeProfile
from src.config import Config
from src.security import validate_input_url, validate_output_path
from src.transcoder.process_manager import (
    NonBlockingFFmpegProcess,
    get_process_manager,
    ProcessStatus
)

logger = logging.getLogger(__name__)

# Get global process manager for concurrency control
process_manager = get_process_manager()


class FFmpegTranscoder:
    def __init__(self, ffmpeg_path: str = None):
        self.ffmpeg_path = ffmpeg_path or Config.FFMPEG_PATH

    def build_command(self, input_url: str, output_urls: Dict[str, str],
                     profiles: List[TranscodeProfile]) -> List[str]:
        validate_input_url(input_url)
        cmd = [self.ffmpeg_path, '-i', input_url]

        for profile in profiles:
            cmd.extend([
                '-c:v', 'libx264',
                '-profile:v', profile.profile,
                '-preset', 'fast',
                '-b:v', str(profile.video_bitrate),
                '-maxrate', str(int(profile.video_bitrate * 1.2)),
                '-bufsize', str(profile.video_bitrate * 2),
                '-r', str(profile.framerate),
                '-s', f"{profile.width}x{profile.height}",
                '-c:a', 'aac',
                '-b:a', str(profile.audio_bitrate),
                '-ar', '48000',
                '-f', 'flv',
                output_urls[profile.name]
            ])

        cmd.extend(['-y'])
        return cmd

    def build_hls_transcode_command(self, input_url: str, output_dir: str,
                                     profile: TranscodeProfile, segment_duration: int = 2) -> List[str]:
        validate_input_url(input_url)
        validate_output_path(output_dir)
        os.makedirs(output_dir, exist_ok=True)
        output_pattern = os.path.join(output_dir, f'{profile.name}_%05d.ts')

        cmd = [
            self.ffmpeg_path,
            '-i', input_url,
            '-c:v', 'libx264',
            '-profile:v', profile.profile,
            '-preset', 'medium',
            '-b:v', str(profile.video_bitrate),
            '-maxrate', str(int(profile.video_bitrate * 1.3)),
            '-bufsize', str(profile.video_bitrate * 2),
            '-r', str(profile.framerate),
            '-s', f"{profile.width}x{profile.height}",
            '-keyint_min', str(profile.framerate * 2),
            '-g', str(profile.framerate * 2),
            '-sc_threshold', '0',
            '-c:a', 'aac',
            '-b:a', str(profile.audio_bitrate),
            '-ar', '48000',
            '-ac', '2',
            '-hls_time', str(segment_duration),
            '-hls_list_size', '10',
            '-hls_wrap', '10',
            '-hls_allow_cache', '1',
            '-hls_segment_filename', output_pattern,
            '-y',
            os.path.join(output_dir, f'{profile.name}.m3u8')
        ]

        return cmd

    def start_transcode(self, cmd: List[str], stream_id: str = None) -> Optional[NonBlockingFFmpegProcess]:
        """
        Start a FFmpeg transcode process with non-blocking output handling.

        Uses global process manager for concurrency control.
        Returns None if concurrency limit would be exceeded.
        """
        process_id = stream_id or f"transcode_{int(time.time() * 1000)}"

        # Define exit callback for logging
        def on_exit(pid: str, exit_code: int):
            if exit_code == 0:
                logger.info(f"Transcode process {pid} completed successfully")
            else:
                logger.error(f"Transcode process {pid} failed with exit code {exit_code}")

        return process_manager.start_process(process_id, cmd, on_exit=on_exit)

    def check_transcode_health(self, process: NonBlockingFFmpegProcess) -> bool:
        """Check if a FFmpeg process is still running healthy"""
        if process is None:
            return False

        if not process.is_running():
            info = process.get_info()
            recent_output = process.get_recent_output(10)
            logger.error(f"FFmpeg exited with code {info.exit_code}")
            if recent_output:
                logger.error(f"Recent stderr output:\n{''.join(recent_output)}")
            return False

        return True

    def stop_transcode(self, process: NonBlockingFFmpegProcess):
        """Gracefully stop a FFmpeg process"""
        if process and process.is_running():
            process.stop(timeout=10)
            logger.info(f"FFmpeg transcoder PID {process.get_pid()} stopped")

    def probe_stream_info(self, input_url: str) -> Optional[Dict]:
        validate_input_url(input_url)
        probe_cmd = [
            self.ffmpeg_path.replace('ffmpeg', 'ffprobe'),
            '-v', 'quiet',
            '-print_format', 'json',
            '-show_format',
            '-show_streams',
            input_url
        ]

        try:
            result = subprocess.run(probe_cmd, capture_output=True, text=True, timeout=30)
            if result.returncode == 0:
                import json
                return json.loads(result.stdout)
            return None
        except Exception as e:
            logger.error(f"Failed to probe stream: {e}")
            return None


class BitrateAdaptor:
    def __init__(self, history_ttl_seconds: int = 3600):
        # Store tuples of (bandwidth, timestamp) for TTL-based cleanup
        self.bandwidth_history: Dict[str, List[tuple[int, float]]] = {}
        self.history_ttl = history_ttl_seconds
        self._last_cleanup = time.time()
        self._cleanup_interval = 600  # 10 minutes

    def update_bandwidth(self, client_id: str, bandwidth: int):
        if client_id not in self.bandwidth_history:
            self.bandwidth_history[client_id] = []

        history = self.bandwidth_history[client_id]
        history.append((bandwidth, time.time()))

        # Keep only last 10 entries per client
        if len(history) > 10:
            history.pop(0)

        # Periodically cleanup expired client records
        self._maybe_cleanup()

    def _maybe_cleanup(self):
        """Clean up expired client records to prevent memory leaks"""
        now = time.time()
        if now - self._last_cleanup < self._cleanup_interval:
            return

        self._last_cleanup = now
        expired_time = now - self.history_ttl

        # Find clients whose last update was longer than TTL ago
        expired_clients = [
            cid for cid, hist in self.bandwidth_history.items()
            if hist and hist[-1][1] < expired_time
        ]

        for cid in expired_clients:
            del self.bandwidth_history[cid]

        if expired_clients:
            logger.info(f"Cleaned up {len(expired_clients)} expired client bandwidth records")

    def get_average_bandwidth(self, client_id: str) -> int:
        history = self.bandwidth_history.get(client_id, [])
        if not history:
            return 5000000  # Default: 5 Mbps
        # Extract just the bandwidth values from tuples
        bandwidth_values = [b for b, _ in history]
        return sum(bandwidth_values) // len(bandwidth_values)

    def recommend_profile(self, client_id: str, available_profiles: List[str]) -> str:
        avg_bw = self.get_average_bandwidth(client_id)

        if avg_bw >= 4500000 and '1080p' in available_profiles:
            return '1080p'
        elif avg_bw >= 2000000 and '720p' in available_profiles:
            return '720p'
        elif avg_bw >= 800000 and '480p' in available_profiles:
            return '480p'
        else:
            return available_profiles[0] if available_profiles else '480p'
