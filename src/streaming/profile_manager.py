import json
import logging
from typing import Dict, List, Optional
from dataclasses import dataclass

logger = logging.getLogger(__name__)


@dataclass
class TranscodeProfile:
    name: str
    width: int
    height: int
    video_bitrate: int
    audio_bitrate: int
    framerate: int
    profile: str

    def to_dict(self) -> Dict:
        return {
            'name': self.name,
            'width': self.width,
            'height': self.height,
            'video_bitrate': self.video_bitrate,
            'audio_bitrate': self.audio_bitrate,
            'framerate': self.framerate,
            'profile': self.profile
        }


class ProfileManager:
    def __init__(self, config_path: str):
        self.config_path = config_path
        self.profiles: Dict[str, TranscodeProfile] = {}
        self._load_profiles()

    def _load_profiles(self):
        try:
            with open(self.config_path, 'r', encoding='utf-8') as f:
                data = json.load(f)

            for name, params in data.items():
                self.profiles[name] = TranscodeProfile(
                    name=name,
                    width=params['width'],
                    height=params['height'],
                    video_bitrate=params['video_bitrate'],
                    audio_bitrate=params['audio_bitrate'],
                    framerate=params['framerate'],
                    profile=params['profile']
                )
            logger.info(f"Loaded {len(self.profiles)} transcode profiles")
        except Exception as e:
            logger.error(f"Failed to load transcode profiles: {e}")

    def get_profile(self, name: str) -> Optional[TranscodeProfile]:
        return self.profiles.get(name)

    def get_all_profiles(self) -> List[TranscodeProfile]:
        return list(self.profiles.values())

    def get_profile_names(self) -> List[str]:
        return list(self.profiles.keys())

    def auto_select_profile(self, bandwidth: int) -> str:
        if bandwidth >= 5000000:
            return '1080p'
        elif bandwidth >= 2500000:
            return '720p'
        elif bandwidth >= 800000:
            return '480p'
        else:
            return list(self.profiles.keys())[0] if self.profiles else '480p'
