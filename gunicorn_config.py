"""
Gunicorn configuration for streaming media server.

Optimized for:
- High concurrent connections
- Long-lived HTTP connections (HLS streaming)
- Low latency response
- CPU affinity for performance
"""
import os
import multiprocessing


# Server socket
bind = "0.0.0.0:8000"
backlog = 2048

# Worker processes
# Rule of thumb: CPU cores * 2 + 1 for non-CPU bound tasks
# For streaming workload (mostly IO bound), slightly higher is okay
workers = min(multiprocessing.cpu_count() * 2 + 1, 8)
worker_class = "gevent"  # Async worker for high concurrency
worker_connections = 1000  # Max concurrent connections per worker
max_requests = 1000  # Restart worker after N requests (prevent memory leaks)
max_requests_jitter = 100  # Randomize restart to avoid thundering herd
timeout = 60  # Seconds - longer for HLS connections
keepalive = 15  # Seconds - keep connections alive

# Worker naming
proc_name = "streaming-server"

# Preload application for better performance (workers share memory)
preload_app = True

# Logging
accesslog = "-"  # Log to stdout
errorlog = "-"   # Log to stderr
loglevel = "info"
access_log_format = '%(h)s %(l)s %(u)s %(t)s "%(r)s" %(s)s %(b)s "%(f)s" "%(a)s" %(L)s'

# SSL (uncomment if terminating SSL at gunicorn - usually better to use nginx)
# keyfile = "/path/to/key.pem"
# certfile = "/path/to/cert.pem"

# Server mechanics
daemon = False  # Don't daemonize (Docker-friendly)
pidfile = None
umask = 0
user = None
group = None
tmp_upload_dir = None

# Debug settings - disable in production
debug = False
spew = False

# Post-fork hook for per-worker initialization
def post_fork(server, worker):
    server.log.info("Worker spawned (pid: %s)", worker.pid)

    # Initialize per-worker resources here if needed
    # e.g., database connections, thread pools

# Pre-exec hook
def pre_exec(server):
    server.log.info("Forked child, re-executing.")

# When server is ready
def when_ready(server):
    server.log.info("Server is ready. Spawning workers.")
    server.log.info(f"Workers: {workers}, Class: {worker_class}, Connections/Worker: {worker_connections}")

# Worker exit hook
def worker_exit(server, worker):
    server.log.info("Worker exited (pid: %s)", worker.pid)
