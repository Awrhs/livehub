"""
Structured logging configuration for streaming server.

Provides JSON-formatted logging with request tracing.
"""
import logging
import json
import time
import uuid
from datetime import datetime
from flask import g, request


class JSONFormatter(logging.Formatter):
    """JSON log formatter with structured fields"""

    def format(self, record):
        """Format log record as JSON"""
        log_entry = {
            "timestamp": datetime.utcnow().isoformat() + "Z",
            "level": record.levelname,
            "logger": record.name,
            "message": record.getMessage(),
            "module": record.module,
            "function": record.funcName,
            "line": record.lineno,
        }

        # Add trace ID if available
        if hasattr(g, 'trace_id'):
            log_entry["trace_id"] = g.trace_id

        # Add request info if in request context
        try:
            if request.path:
                log_entry["path"] = request.path
                log_entry["method"] = request.method
                log_entry["remote_addr"] = request.remote_addr
        except RuntimeError:
            pass  # Not in request context

        # Add exception info if present
        if record.exc_info:
            log_entry["exception"] = self.formatException(record.exc_info)

        # Add extra attributes
        for attr in ['stream_id', 'client_id', 'task_id']:
            if hasattr(record, attr):
                log_entry[attr] = getattr(record, attr)

        return json.dumps(log_entry)


def setup_logging(level=logging.INFO):
    """Setup root logger with JSON formatting"""
    root_logger = logging.getLogger()
    root_logger.setLevel(level)

    # Remove existing handlers
    for handler in list(root_logger.handlers):
        root_logger.removeHandler(handler)

    # Add console handler with JSON format
    console_handler = logging.StreamHandler()
    console_handler.setFormatter(JSONFormatter())
    root_logger.addHandler(console_handler)


def setup_request_tracing(app):
    """Add request tracing middleware to Flask app"""

    @app.before_request
    def assign_trace_id():
        """Assign unique trace ID to each request and log start"""
        g.trace_id = str(uuid.uuid4())
        g.start_time = time.time()

        logging.getLogger("request").debug(
            f"Request started: {request.method} {request.path}",
            extra={
                'trace_id': g.trace_id,
                'method': request.method,
                'path': request.path,
                'remote_addr': request.remote_addr,
                'user_agent': request.user_agent.string
            }
        )

    @app.after_request
    def log_request_completion(response):
        """Log request completion with timing"""
        duration = time.time() - g.start_time

        logging.getLogger("request").info(
            f"Request completed: {request.method} {request.path} {response.status_code}",
            extra={
                'trace_id': g.trace_id,
                'status_code': response.status_code,
                'duration_ms': round(duration * 1000, 2),
                'method': request.method,
                'path': request.path
            }
        )

        return response
