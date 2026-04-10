from functools import wraps
from flask import request, jsonify
from jose import JWTError, jwt
from typing import Callable
from src.config import Config


class JWTAuthenticator:
    def __init__(self, secret_key: str = None):
        self.secret_key = secret_key or Config.JWT_SECRET
        self.algorithm = 'HS256'

    def verify_token(self, token: str) -> dict:
        try:
            payload = jwt.decode(token, self.secret_key, algorithms=[self.algorithm])
            return payload
        except JWTError:
            return None

    def auth_required(self, f: Callable) -> Callable:
        @wraps(f)
        def decorated_function(*args, **kwargs):
            if not Config.ENABLE_AUTH:
                return f(*args, **kwargs)

            auth_header = request.headers.get('Authorization')
            if not auth_header or not auth_header.startswith('Bearer '):
                return jsonify({'error': 'Missing or invalid authorization token'}), 401

            token = auth_header.split(' ')[1]
            payload = self.verify_token(token)
            if not payload:
                return jsonify({'error': 'Invalid or expired token'}), 401

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
        payload = self.verify_token(token)
        if not payload:
            return False
        return payload.get('stream_id') == stream_id
