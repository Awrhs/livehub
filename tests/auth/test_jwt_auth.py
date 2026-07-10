import time
import pytest
from unittest.mock import patch, MagicMock

from src.auth.jwt_auth import (
    JWTAuthenticator,
    TokenExpiredError,
    InvalidTokenError
)


class TestJWTAuthenticator:
    """Test suite for JWTAuthenticator class"""

    TEST_SECRET = "test-secret-key-12345-67890"

    def test_init_default_secret(self):
        """Test initialization with default secret from config"""
        with patch('src.auth.jwt_auth.Config') as mock_config:
            mock_config.JWT_SECRET = self.TEST_SECRET
            auth = JWTAuthenticator()
            assert auth.secret_key == self.TEST_SECRET
            assert auth.algorithm == 'HS256'

    def test_init_custom_secret(self):
        """Test initialization with custom secret"""
        auth = JWTAuthenticator(secret_key="custom-secret")
        assert auth.secret_key == "custom-secret"

    def test_verify_token_valid(self):
        """Test verifying a valid token"""
        auth = JWTAuthenticator(secret_key=self.TEST_SECRET)
        token = auth.generate_m3u8_token("stream1", "127.0.0.1")

        payload = auth.verify_token(token)
        assert payload is not None
        assert payload['stream_id'] == "stream1"
        assert payload['client_ip'] == "127.0.0.1"

    def test_verify_token_expired(self):
        """Test verifying an expired token"""
        auth = JWTAuthenticator(secret_key=self.TEST_SECRET)

        # Generate token that expired 1 second ago
        expired_time = int(time.time()) - 1
        with patch('src.auth.jwt_auth.jwt.encode') as mock_encode:
            mock_encode.return_value = "expired-token"
            with patch('src.auth.jwt_auth.jwt.decode') as mock_decode:
                from jose.exceptions import ExpiredSignatureError
                mock_decode.side_effect = ExpiredSignatureError("Token expired")

                with pytest.raises(TokenExpiredError):
                    auth.verify_token("expired-token")

    def test_verify_token_invalid_signature(self):
        """Test verifying a token with invalid signature"""
        auth1 = JWTAuthenticator(secret_key="secret1")
        auth2 = JWTAuthenticator(secret_key="secret2")

        # Generate token with auth1, verify with auth2 (different secret)
        token = auth1.generate_m3u8_token("stream1", "127.0.0.1")

        with pytest.raises(InvalidTokenError):
            auth2.verify_token(token)

    def test_verify_token_malformed(self):
        """Test verifying a malformed token"""
        auth = JWTAuthenticator(secret_key=self.TEST_SECRET)

        with pytest.raises(InvalidTokenError):
            auth.verify_token("malformed-token")

    def test_generate_m3u8_token(self):
        """Test generating m3u8 token"""
        auth = JWTAuthenticator(secret_key=self.TEST_SECRET)

        token = auth.generate_m3u8_token("test-stream", "192.168.1.1", expiration=3600)

        assert isinstance(token, str)
        assert len(token) > 0

        # Verify token content
        payload = auth.verify_token(token)
        assert payload['stream_id'] == "test-stream"
        assert payload['client_ip'] == "192.168.1.1"
        assert 'exp' in payload

    def test_verify_m3u8_token_valid(self):
        """Test verifying a valid m3u8 token"""
        auth = JWTAuthenticator(secret_key=self.TEST_SECRET)
        token = auth.generate_m3u8_token("stream123", "10.0.0.1")

        result = auth.verify_m3u8_token(token, "stream123", "10.0.0.1")
        assert result is True

    def test_verify_m3u8_token_wrong_stream(self):
        """Test verifying token with wrong stream_id"""
        auth = JWTAuthenticator(secret_key=self.TEST_SECRET)
        token = auth.generate_m3u8_token("stream1", "127.0.0.1")

        result = auth.verify_m3u8_token(token, "wrong-stream", "127.0.0.1")
        assert result is False

    def test_verify_m3u8_token_wrong_ip(self):
        """Test verifying token with wrong client_ip"""
        auth = JWTAuthenticator(secret_key=self.TEST_SECRET)
        token = auth.generate_m3u8_token("stream1", "127.0.0.1")

        result = auth.verify_m3u8_token(token, "stream1", "192.168.1.1")
        assert result is False

    def test_verify_m3u8_token_expired(self):
        """Test verifying an expired m3u8 token"""
        auth = JWTAuthenticator(secret_key=self.TEST_SECRET)

        with patch('src.auth.jwt_auth.jwt.decode') as mock_decode:
            from jose.exceptions import ExpiredSignatureError
            mock_decode.side_effect = ExpiredSignatureError()

            result = auth.verify_m3u8_token("expired-token", "stream1", "127.0.0.1")
            assert result is False

    def test_auth_required_decorator_disabled(self):
        """Test auth_required decorator when auth is disabled"""
        with patch('src.auth.jwt_auth.Config') as mock_config:
            mock_config.ENABLE_AUTH = False
            auth = JWTAuthenticator(secret_key=self.TEST_SECRET)

            mock_func = MagicMock(return_value="success")
            decorated = auth.auth_required(mock_func)

            result = decorated()
            assert result == "success"
            mock_func.assert_called_once()


class TestCustomExceptions:
    """Test custom exception types"""

    def test_token_expired_error(self):
        """Test TokenExpiredError"""
        with pytest.raises(TokenExpiredError) as exc_info:
            raise TokenExpiredError("Token has expired")
        assert "expired" in str(exc_info.value).lower()

    def test_invalid_token_error(self):
        """Test InvalidTokenError"""
        with pytest.raises(InvalidTokenError) as exc_info:
            raise InvalidTokenError("Invalid signature")
        assert "invalid" in str(exc_info.value).lower()

    def test_exception_hierarchy(self):
        """Test exceptions are properly raised"""
        # Both should be Exception subtypes
        assert isinstance(TokenExpiredError(), Exception)
        assert isinstance(InvalidTokenError(), Exception)
