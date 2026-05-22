import os
import logging
from typing import List, Optional, Dict
from dataclasses import dataclass

from src.security import safe_media_path, validate_safe_name

logger = logging.getLogger(__name__)


@dataclass
class MediaPlaylistItem:
    duration: float
    filename: str
    is_discontinuity: bool = False


class HLSGenerator:
    def __init__(self, base_path: str, fragment_duration: int = 2, window_size: int = 10):
        self.base_path = base_path
        self.fragment_duration = fragment_duration
        self.window_size = window_size

    def generate_master_playlist(self, stream_id: str, profiles: List[Dict]) -> str:
        lines = ['#EXTM3U', '#EXT-X-VERSION:3']

        for profile in profiles:
            bandwidth = profile.get('video_bitrate', 800000)
            resolution = f"{profile['width']}x{profile['height']}"
            profile_name = profile['name']

            lines.append(f'#EXT-X-STREAM-INF:BANDWIDTH={bandwidth},RESOLUTION={resolution}')
            lines.append(f'{stream_id}/{profile_name}.m3u8')

        return '\n'.join(lines)

    def generate_media_playlist(self, items: List[MediaPlaylistItem]) -> str:
        lines = ['#EXTM3U', '#EXT-X-VERSION:3']
        lines.append(f'#EXT-X-TARGETDURATION:{self.fragment_duration}')

        for item in items:
            if item.is_discontinuity:
                lines.append('#EXT-X-DISCONTINUITY')
            lines.append(f'#EXTINF:{item.duration},')
            lines.append(item.filename)

        if items:
            lines.append('#EXT-X-ENDLIST')

        return '\n'.join(lines)

    def write_playlist(self, content: str, stream_id: str, filename: str) -> bool:
        try:
            stream_id = validate_safe_name(stream_id, 'stream_id')
            filename = validate_safe_name(filename, 'filename')
            full_path = safe_media_path(self.base_path, stream_id)
            os.makedirs(full_path, exist_ok=True)

            output_path = safe_media_path(full_path, filename)
            with open(output_path, 'w', encoding='utf-8') as f:
                f.write(content)

            logger.debug(f"Written HLS playlist to {output_path}")
            return True
        except Exception as e:
            logger.error(f"Failed to write HLS playlist: {e}")
            return False

    def get_playlist_path(self, stream_id: str, profile: str = None) -> str:
        stream_id = validate_safe_name(stream_id, 'stream_id')
        if profile:
            profile = validate_safe_name(profile, 'profile')
            return safe_media_path(self.base_path, stream_id, f"{profile}.m3u8")
        return safe_media_path(self.base_path, f"{stream_id}.m3u8")


class HLSPlayListChecker:
    def __init__(self, base_path: str):
        self.base_path = base_path

    def verify_ts_file(self, ts_path: str) -> bool:
        if not os.path.exists(ts_path):
            return False

        file_size = os.path.getsize(ts_path)
        if file_size < 188:
            return False

        return True

    def check_integrity(self, playlist_path: str) -> Dict:
        result = {
            'valid': True,
            'missing_files': [],
            'corrupted_files': [],
            'total_segments': 0
        }

        if not os.path.exists(playlist_path):
            result['valid'] = False
            return result

        with open(playlist_path, 'r', encoding='utf-8') as f:
            lines = f.readlines()

        playlist_dir = os.path.dirname(playlist_path)

        for line in lines:
            line = line.strip()
            if not line or line.startswith('#'):
                continue

            result['total_segments'] += 1
            ts_path = os.path.join(playlist_dir, line)

            if not os.path.exists(ts_path):
                result['valid'] = False
                result['missing_files'].append(line)
            elif not self.verify_ts_file(ts_path):
                result['valid'] = False
                result['corrupted_files'].append(line)

        return result
