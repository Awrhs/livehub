import logging
from functools import wraps
from flask import request, jsonify, g
from jose import JWTError, jwt
from jose.exceptions import ExpiredSignatureError
from typing import Callable
from src.config import Config
from src.cache.redis_client import get_redis_client


logger = logging.getLogger(__name__)
redis_client = get_redis_client()


class TokenExpiredError(Exception):
    """Raised when the JWT token has expired"""
    pass


class InvalidTokenError(Exception):
    """Raised when the JWT token is invalid (bad signature, malformed, etc.)"""
    pass


class JWTAuthenticator:
    def __init__(self, secret_key: str = None):
        self.secret_key = secret_key or Config.JWT_SECRET
        self.algorithm = 'HS256'

    def verify_token(self, token: str) -> dict:
        # Check blacklist first
        if redis_client.is_token_blacklisted(token):
            logger.warning(f"Token was revoked: {token[:10]}...")
            raise InvalidTokenError("Token has been revoked")

        try:
            payload = jwt.decode(token, self.secret_key, algorithms=[self.algorithm])
            return payload
        except ExpiredSignatureError:
            logger.warning(f"Token expired: {token[:10]}...")
            raise TokenExpiredError("Token has expired")
        except JWTError as e:
            logger.warning(f"Invalid token: {str(e)}")
            raise InvalidTokenError(f"Invalid token: {str(e)}") from e

    def revoke_token(self, token: str) -> bool:
        """Revoke a token by adding it to the blacklist"""
        try:
            payload = jwt.decode(token, self.secret_key, algorithms=[self.algorithm],
                               options={"verify_exp": False})
            expires_at = payload.get('exp', 0)
            return redis_client.blacklist_token(token, expires_at)
        except JWTError:
            return False

    def auth_required(self, f: Callable) -> Callable:
        @wraps(f)
        def decorated_function(*args, **kwargs):
            if not Config.ENABLE_AUTH:
                return f(*args, **kwargs)

            auth_header = request.headers.get('Authorization')
            token = request.args.get('token')
            if auth_header and auth_header.startswith('Bearer '):
                token = auth_header.split(' ', 1)[1]
            if not token:
                return jsonify({'error': 'Missing or invalid authorization token'}), 401

            try:
                payload = self.verify_token(token)
            except (TokenExpiredError, InvalidTokenError) as e:
                logger.info(f"Authentication failed: {str(e)}")
                return jsonify({'error': str(e)}), 401

            request.user = payload
            return f(*args, **kwargs)

        return decorated_function

    def generate_m3u8_token(self, stream_id: str, client_ip: str, expiration: int = 3600) -> str:
        import time
        payload = {
            'stream_id': stream_id,
            'client_ip': client_ip,
            'exp': int(time.time()) + expiration
        }
        return jwt.encode(payload, self.secret_key, algorithm=self.algorithm)

    def verify_m3u8_token(self, token: str, stream_id: str, client_ip: str) -> bool:
        try:
            payload = self.verify_token(token)
        except (TokenExpiredError, InvalidTokenError):
            return False

        token_ip = payload.get('client_ip')
        return payload.get('stream_id') == stream_id and (not token_ip or token_ip == client_ip)
