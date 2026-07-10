"""
FFmpeg Process Manager with non-blocking output handling.

This module provides:
1. Non-blocking output reading (prevents pipe buffer deadlock)
2. Process health monitoring
3. Automatic cleanup of zombie processes
4. Line-by-line output logging
"""
import threading
import queue
import subprocess
import time
import logging
import os
import signal
from typing import List, Optional, Dict, Callable
from dataclasses import dataclass
from enum import Enum


logger = logging.getLogger(__name__)


class ProcessStatus(Enum):
    """FFmpeg process status"""
    RUNNING = "running"
    STOPPED = "stopped"
    FAILED = "failed"
    ZOMBIE = "zombie"


@dataclass
class ProcessInfo:
    """Information about a running FFmpeg process"""
    process: subprocess.Popen
    cmd: List[str]
    start_time: float
    status: ProcessStatus
    output_lines: List[str]
    error_lines: List[str]
    exit_code: Optional[int] = None
    pid: Optional[int] = None

    @property
    def runtime_seconds(self) -> float:
        return time.time() - self.start_time


class NonBlockingFFmpegProcess:
    """
    Wrapper for FFmpeg process that reads output asynchronously.

    Prevents pipe buffer deadlock by reading stdout/stderr in background threads.
    """

    def __init__(self, cmd: List[str], on_exit: Optional[Callable[[str, int], None]] = None):
        self.cmd = cmd
        self.on_exit = on_exit
        self._process: Optional[subprocess.Popen] = None
        self._output_queue: queue.Queue = queue.Queue(maxsize=1000)
        self._reader_threads: List[threading.Thread] = []
        self._stop_event = threading.Event()
        self._stdout_lines: List[str] = []
        self._stderr_lines: List[str] = []
        self._start_time: float = 0
        self._lock = threading.Lock()

    def start(self) -> "NonBlockingFFmpegProcess":
        """Start the FFmpeg process and background reader threads"""
        logger.info(f"Starting FFmpeg: {' '.join(self.cmd)}")

        self._process = subprocess.Popen(
            self.cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=1,
            universal_newlines=True,
            preexec_fn=os.setsid  # Create new process group for clean termination
        )
        self._start_time = time.time()

        # Start background reader threads
        self._reader_threads = [
            threading.Thread(
                target=self._read_stream,
                args=(self._process.stdout, "stdout"),
                daemon=True,
                name=f"ffmpeg-reader-{self._process.pid}-stdout"
            ),
            threading.Thread(
                target=self._read_stream,
                args=(self._process.stderr, "stderr"),
                daemon=True,
                name=f"ffmpeg-reader-{self._process.pid}-stderr"
            )
        ]

        for t in self._reader_threads:
            t.start()

        logger.info(f"FFmpeg process started with PID {self._process.pid}")
        return self

    def _read_stream(self, stream, stream_name: str):
        """Background thread function to read output stream without blocking"""
        try:
            for line in iter(stream.readline, ''):
                if self._stop_event.is_set():
                    break

                line = line.rstrip('\n')
                if line:
                    # Put in queue for processing, drop oldest if full
                    try:
                        self._output_queue.put_nowait((stream_name, line, time.time()))
                    except queue.Full:
                        # Queue full, drop the oldest entry and add new one
                        try:
                            self._output_queue.get_nowait()
                            self._output_queue.put_nowait((stream_name, line, time.time()))
                        except queue.Empty:
                            pass

                    # Store last N lines
                    with self._lock:
                        lines = self._stdout_lines if stream_name == "stdout" else self._stderr_lines
                        lines.append(line)
                        # Keep only last 200 lines to avoid memory leak
                        if len(lines) > 200:
                            lines[:] = lines[-200:]

        except Exception as e:
            logger.debug(f"Error reading FFmpeg {stream_name}: {e}")
        finally:
            stream.close()

    def poll(self) -> Optional[int]:
        """Check if process has exited, return exit code or None if running"""
        if self._process is None:
            return None
        return self._process.poll()

    def is_running(self) -> bool:
        """Check if process is still running"""
        return self.poll() is None

    def get_pid(self) -> Optional[int]:
        """Get process PID"""
        return self._process.pid if self._process else None

    def get_runtime(self) -> float:
        """Get process runtime in seconds"""
        if self._start_time == 0:
            return 0
        return time.time() - self._start_time

    def get_recent_output(self, lines: int = 10) -> List[str]:
        """Get most recent output lines"""
        with self._lock:
            return list(self._stderr_lines[-lines:])  # FFmpeg mostly logs to stderr

    def stop(self, timeout: int = 10) -> bool:
        """
        Gracefully stop the FFmpeg process.

        First tries SIGTERM, then SIGKILL if process doesn't exit within timeout.
        """
        if self._process is None:
            return True

        self._stop_event.set()

        # Signal the entire process group
        try:
            os.killpg(os.getpgid(self._process.pid), signal.SIGTERM)
        except (OSError, ProcessLookupError):
            pass

        # Wait for readers and process
        try:
            for t in self._reader_threads:
                t.join(timeout=1)

            # Wait for process to exit
            try:
                self._process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                logger.warning(f"FFmpeg PID {self._process.pid} did not exit gracefully, killing...")
                try:
                    os.killpg(os.getpgid(self._process.pid), signal.SIGKILL)
                except (OSError, ProcessLookupError):
                    pass
                self._process.wait()

            # Call exit callback if registered
            if self.on_exit:
                try:
                    self.on_exit(str(self._process.pid), self._process.returncode)
                except Exception as e:
                    logger.error(f"Error in on_exit callback: {e}")

            logger.info(f"FFmpeg PID {self._process.pid} stopped with code {self._process.returncode}")
            return True

        except Exception as e:
            logger.error(f"Error stopping FFmpeg process: {e}")
            return False

    def get_info(self) -> ProcessInfo:
        """Get full process information"""
        with self._lock:
            return ProcessInfo(
                process=self._process,
                cmd=self.cmd,
                start_time=self._start_time,
                status=self._get_status(),
                output_lines=list(self._stdout_lines),
                error_lines=list(self._stderr_lines),
                exit_code=self._process.returncode if self._process and self._process.poll() is not None else None,
                pid=self._process.pid if self._process else None
            )

    def _get_status(self) -> ProcessStatus:
        if self._process is None:
            return ProcessStatus.STOPPED

        return_code = self._process.poll()
        if return_code is None:
            return ProcessStatus.RUNNING
        elif return_code == 0:
            return ProcessStatus.STOPPED
        else:
            return ProcessStatus.FAILED


class FFmpegProcessManager:
    """
    Central manager for all FFmpeg processes.

    Provides:
    - Process registration and tracking
    - Health monitoring
    - Zombie process cleanup
    - Concurrency limits
    """

    def __init__(self, max_concurrent: Optional[int] = None):
        import multiprocessing
        self.max_concurrent = max_concurrent or (multiprocessing.cpu_count() or 4)
        self._processes: Dict[str, NonBlockingFFmpegProcess] = {}
        self._lock = threading.Lock()
        self._monitor_thread: Optional[threading.Thread] = None
        self._stop_monitor = threading.Event()
        self._start_monitor()

    def _start_monitor(self):
        """Start background monitor thread"""
        self._monitor_thread = threading.Thread(
            target=self._monitor_loop,
            daemon=True,
            name="ffmpeg-monitor"
        )
        self._monitor_thread.start()
        logger.info("FFmpeg process manager monitor started")

    def _monitor_loop(self):
        """Background thread that monitors process health and cleans up zombies"""
        while not self._stop_monitor.is_set():
            try:
                self._cleanup_exited()
                self._check_for_zombies()
            except Exception as e:
                logger.error(f"Error in process monitor loop: {e}")

            time.sleep(5)  # Check every 5 seconds

    def _cleanup_exited(self):
        """Remove exited processes from tracking"""
        with self._lock:
            exited = [
                pid for pid, proc in self._processes.items()
                if not proc.is_running()
            ]
            for pid in exited:
                del self._processes[pid]
                logger.debug(f"Cleaned up exited FFmpeg process {pid}")

    def _check_for_zombies(self):
        """Check for and kill zombie FFmpeg processes"""
        # This is more advanced - would require scanning /proc or similar
        # For now, we rely on proper stop() calls
        pass

    def can_start_process(self) -> bool:
        """Check if we can start another FFmpeg process"""
        import psutil

        with self._lock:
            running_count = sum(1 for p in self._processes.values() if p.is_running())

        if running_count >= self.max_concurrent:
            return False

        # Check CPU load
        try:
            cpu_percent = psutil.cpu_percent(interval=0.1)
            if cpu_percent > 85:  # >85% CPU, don't start new transcode
                logger.warning(f"CPU load too high ({cpu_percent}%), rejecting new transcode")
                return False
        except Exception:
            pass

        return True

    def start_process(self, process_id: str, cmd: List[str],
                     on_exit: Optional[Callable[[str, int], None]] = None) -> Optional[NonBlockingFFmpegProcess]:
        """
        Start a new FFmpeg process.

        Returns None if concurrency limit would be exceeded.
        """
        if not self.can_start_process():
            logger.warning(f"Cannot start new FFmpeg process: concurrency limit reached")
            return None

        proc = NonBlockingFFmpegProcess(cmd, on_exit=on_exit).start()

        with self._lock:
            self._processes[process_id] = proc

        return proc

    def stop_process(self, process_id: str) -> bool:
        """Stop a specific FFmpeg process"""
        with self._lock:
            proc = self._processes.get(process_id)

        if proc is None:
            return False

        result = proc.stop()

        with self._lock:
            if process_id in self._processes:
                del self._processes[process_id]

        return result

    def get_process(self, process_id: str) -> Optional[NonBlockingFFmpegProcess]:
        """Get a specific process by ID"""
        with self._lock:
            return self._processes.get(process_id)

    def get_all_processes(self) -> Dict[str, NonBlockingFFmpegProcess]:
        """Get all tracked processes"""
        with self._lock:
            return dict(self._processes)

    def get_running_count(self) -> int:
        """Get count of running FFmpeg processes"""
        with self._lock:
            return sum(1 for p in self._processes.values() if p.is_running())

    def shutdown(self):
        """Shutdown manager and stop all processes"""
        logger.info("Shutting down FFmpeg process manager...")
        self._stop_monitor.set()

        # Stop all processes
        with self._lock:
            for pid, proc in list(self._processes.items()):
                try:
                    proc.stop(timeout=5)
                except Exception as e:
                    logger.error(f"Error stopping process {pid}: {e}")

            self._processes.clear()

        logger.info("FFmpeg process manager shutdown complete")


# Global singleton instance
_process_manager: Optional[FFmpegProcessManager] = None


def get_process_manager(max_concurrent: Optional[int] = None) -> FFmpegProcessManager:
    """Get or create the global FFmpeg process manager"""
    global _process_manager
    if _process_manager is None:
        _process_manager = FFmpegProcessManager(max_concurrent=max_concurrent)
    return _process_manager
