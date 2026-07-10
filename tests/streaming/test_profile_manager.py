import json
import pytest
import tempfile
import os

from src.streaming.profile_manager import (
    ProfileManager,
    TranscodeProfile
)


class TestTranscodeProfile:
    """Test TranscodeProfile dataclass"""

    def test_transcode_profile_creation(self):
        """Test creating a TranscodeProfile"""
        profile = TranscodeProfile(
            name='1080p',
            width=1920,
            height=1080,
            video_bitrate=4500000,
            audio_bitrate=192000,
            framerate=25,
            profile='high'
        )
        assert profile.name == '1080p'
        assert profile.width == 1920
        assert profile.height == 1080

    def test_to_dict(self):
        """Test converting profile to dict"""
        profile = TranscodeProfile(
            name='720p',
            width=1280,
            height=720,
            video_bitrate=2500000,
            audio_bitrate=128000,
            framerate=30,
            profile='main'
        )
        d = profile.to_dict()
        assert d['name'] == '720p'
        assert d['width'] == 1280
        assert d['height'] == 720


class TestProfileManager:
    """Test ProfileManager class"""

    @pytest.fixture
    def valid_config_file(self):
        """Create a temporary valid config file"""
        config = {
            "1080p": {
                "width": 1920,
                "height": 1080,
                "video_bitrate": 4500000,
                "audio_bitrate": 192000,
                "framerate": 25,
                "profile": "high"
            },
            "720p": {
                "width": 1280,
                "height": 720,
                "video_bitrate": 2500000,
                "audio_bitrate": 128000,
                "framerate": 25,
                "profile": "main"
            },
            "480p": {
                "width": 854,
                "height": 480,
                "video_bitrate": 800000,
                "audio_bitrate": 128000,
                "framerate": 25,
                "profile": "baseline"
            }
        }

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            filename = f.name

        yield filename

        os.unlink(filename)

    @pytest.fixture
    def invalid_json_file(self):
        """Create a temporary invalid JSON file"""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            f.write("{ invalid json }")
            filename = f.name

        yield filename

        os.unlink(filename)

    @pytest.fixture
    def missing_field_config_file(self):
        """Create a config file with missing required field"""
        config = {
            "1080p": {
                "width": 1920,
                # Missing height
                "video_bitrate": 4500000,
                "audio_bitrate": 192000,
                "framerate": 25,
                "profile": "high"
            }
        }

        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(config, f)
            filename = f.name

        yield filename

        os.unlink(filename)

    def test_load_valid_config(self, valid_config_file):
        """Test loading a valid config file"""
        manager = ProfileManager(valid_config_file)
        assert len(manager.profiles) == 3
        assert '1080p' in manager.profiles
        assert '720p' in manager.profiles
        assert '480p' in manager.profiles

        # Verify profile values
        profile_1080p = manager.get_profile('1080p')
        assert profile_1080p.width == 1920
        assert profile_1080p.height == 1080
        assert profile_1080p.video_bitrate == 4500000

    def test_file_not_found_raises(self):
        """Test that non-existent config file raises RuntimeError"""
        with pytest.raises(RuntimeError) as exc_info:
            ProfileManager('/nonexistent/path/config.json')
        assert "not found" in str(exc_info.value).lower()

    def test_invalid_json_raises(self, invalid_json_file):
        """Test that invalid JSON raises RuntimeError"""
        with pytest.raises(RuntimeError) as exc_info:
            ProfileManager(invalid_json_file)
        assert "json" in str(exc_info.value).lower()

    def test_missing_field_raises(self, missing_field_config_file):
        """Test that missing required field raises RuntimeError"""
        with pytest.raises(RuntimeError) as exc_info:
            ProfileManager(missing_field_config_file)
        assert "missing" in str(exc_info.value).lower()

    def test_get_profile(self, valid_config_file):
        """Test get_profile method"""
        manager = ProfileManager(valid_config_file)

        profile = manager.get_profile('720p')
        assert profile is not None
        assert profile.name == '720p'

        profile = manager.get_profile('nonexistent')
        assert profile is None

    def test_get_all_profiles(self, valid_config_file):
        """Test get_all_profiles method"""
        manager = ProfileManager(valid_config_file)
        profiles = manager.get_all_profiles()
        assert len(profiles) == 3
        assert all(isinstance(p, TranscodeProfile) for p in profiles)

    def test_get_profile_names(self, valid_config_file):
        """Test get_profile_names method"""
        manager = ProfileManager(valid_config_file)
        names = manager.get_profile_names()
        assert len(names) == 3
        assert '1080p' in names
        assert '720p' in names
        assert '480p' in names

    def test_auto_select_profile_thresholds(self, valid_config_file):
        """Test auto_select_profile bandwidth thresholds"""
        manager = ProfileManager(valid_config_file)

        # Test thresholds aligned with BitrateAdaptor (4.5Mbps / 2Mbps / 800kbps)
        assert manager.auto_select_profile(5000000) == '1080p'  # 5 Mbps
        assert manager.auto_select_profile(4500000) == '1080p'  # 4.5 Mbps - threshold
        assert manager.auto_select_profile(4499999) == '720p'   # Below 4.5 Mbps
        assert manager.auto_select_profile(2500000) == '720p'   # 2.5 Mbps
        assert manager.auto_select_profile(2000000) == '720p'   # 2 Mbps - threshold
        assert manager.auto_select_profile(1999999) == '480p'   # Below 2 Mbps
        assert manager.auto_select_profile(800000) == '480p'    # 800 kbps - threshold
        assert manager.auto_select_profile(799999) == '480p'    # Below 800 kbps
