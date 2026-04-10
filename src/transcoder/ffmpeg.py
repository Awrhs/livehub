import subprocess
import logging
import os
from typing import List, Dict, Optional
from src.streaming.profile_manager import TranscodeProfile
from src.config import Config

logger = logging.getLogger(__name__)


class FFmpegTranscoder:
    def __init__(self, ffmpeg_path: str = None):
        self.ffmpeg_path = ffmpeg_path or Config.FFMPEG_PATH

    def build_command(self, input_url: str, output_urls: Dict[str, str],
                     profiles: List[TranscodeProfile]) -> List[str]:
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

    def start_transcode(self, cmd: List[str]) -> subprocess.Popen:
        logger.info(f"Starting FFmpeg: {' '.join(cmd)}")
        return subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True
        )

    def check_transcode_health(self, process: subprocess.Popen) -> bool:
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            logger.error(f"FFmpeg exited with code {process.returncode}")
            logger.error(f"Stderr: {stderr}")
            return False
        return True

    def stop_transcode(self, process: subprocess.Popen):
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
            logger.info("FFmpeg transcoder stopped")

    def probe_stream_info(self, input_url: str) -> Optional[Dict]:
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
    def __init__(self):
        self.bandwidth_history: Dict[str, List[int]] = {}

    def update_bandwidth(self, client_id: str, bandwidth: int):
        if client_id not in self.bandwidth_history:
            self.bandwidth_history[client_id] = []

        history = self.bandwidth_history[client_id]
        history.append(bandwidth)
        if len(history) > 10:
            history.pop(0)

    def get_average_bandwidth(self, client_id: str) -> int:
        history = self.bandwidth_history.get(client_id, [])
        if not history:
            return 5000000
        return sum(history) // len(history)

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
