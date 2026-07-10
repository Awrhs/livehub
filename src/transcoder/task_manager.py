"""
Transcode Task Manager with queuing and monitoring.

Provides:
- Task queue for pending transcodes when concurrency limit is hit
- Task status tracking
- Priority-based scheduling
- Transcode statistics
"""
import threading
import time
import queue
import logging
from typing import Dict, List, Optional, Callable
from dataclasses import dataclass, field
from enum import Enum
from uuid import uuid4

from src.transcoder.process_manager import (
    FFmpegProcessManager,
    NonBlockingFFmpegProcess,
    get_process_manager,
    ProcessStatus
)


logger = logging.getLogger(__name__)


class TaskStatus(Enum):
    """Status of a transcode task"""
    PENDING = "pending"       # Waiting in queue
    RUNNING = "running"       # Currently running
    COMPLETED = "completed"   # Finished successfully
    FAILED = "failed"         # Failed with error
    CANCELLED = "cancelled"   # Cancelled by user


@dataclass
class TranscodeTask:
    """A transcode task with metadata"""
    task_id: str
    stream_id: str
    cmd: List[str]
    priority: int = 0  # Higher = more important
    status: TaskStatus = TaskStatus.PENDING
    process: Optional[NonBlockingFFmpegProcess] = None
    error_message: Optional[str] = None
    created_at: float = field(default_factory=time.time)
    started_at: Optional[float] = None
    completed_at: Optional[float] = None

    @property
    def queue_time_seconds(self) -> float:
        if self.started_at is None:
            return time.time() - self.created_at
        return self.started_at - self.created_at

    @property
    def runtime_seconds(self) -> Optional[float]:
        if self.started_at is None:
            return None
        end_time = self.completed_at or time.time()
        return end_time - self.started_at


class TranscodeTaskManager:
    """
    Manager for transcode tasks with queuing and concurrency control.

    This sits on top of FFmpegProcessManager and adds:
    - Task queuing when limit is reached
    - Priority scheduling
    - Task lifecycle tracking
    - Statistics
    """

    def __init__(self, max_concurrent: Optional[int] = None, queue_size: int = 100):
        self.process_manager: FFmpegProcessManager = get_process_manager(max_concurrent)
        self.max_concurrent = self.process_manager.max_concurrent
        self._tasks: Dict[str, TranscodeTask] = {}
        self._task_queue: queue.PriorityQueue = queue.PriorityQueue(maxsize=queue_size)
        self._lock = threading.Lock()
        self._stop_event = threading.Event()

        # Statistics
        self._stats = {
            'total_submitted': 0,
            'total_completed': 0,
            'total_failed': 0,
            'total_cancelled': 0,
        }

        # Start worker thread
        self._worker_thread = threading.Thread(
            target=self._worker_loop,
            daemon=True,
            name="transcode-worker"
        )
        self._worker_thread.start()

        logger.info(f"Transcode task manager started with {self.max_concurrent} max concurrent processes")

    def submit_task(self, stream_id: str, cmd: List[str], priority: int = 0) -> str:
        """
        Submit a new transcode task.

        Returns task_id. Raises queue.Full if queue is at capacity.
        """
        task_id = str(uuid4())

        task = TranscodeTask(
            task_id=task_id,
            stream_id=stream_id,
            cmd=cmd,
            priority=priority
        )

        with self._lock:
            self._tasks[task_id] = task
            self._stats['total_submitted'] += 1

        # Use negative priority because PriorityQueue is lowest first
        self._task_queue.put_nowait((-priority, task_id))

        logger.info(f"Submitted transcode task {task_id} for stream {stream_id} (priority {priority})")
        return task_id

    def get_task_status(self, task_id: str) -> Optional[TranscodeTask]:
        """Get status of a specific task"""
        with self._lock:
            return self._tasks.get(task_id)

    def get_all_tasks(self) -> List[TranscodeTask]:
        """Get all tasks"""
        with self._lock:
            return list(self._tasks.values())

    def get_running_tasks(self) -> List[TranscodeTask]:
        """Get all currently running tasks"""
        with self._lock:
            return [
                t for t in self._tasks.values()
                if t.status == TaskStatus.RUNNING
            ]

    def get_pending_tasks(self) -> List[TranscodeTask]:
        """Get all pending tasks in queue"""
        with self._lock:
            return [
                t for t in self._tasks.values()
                if t.status == TaskStatus.PENDING
            ]

    def cancel_task(self, task_id: str) -> bool:
        """Cancel a pending or running task"""
        with self._lock:
            task = self._tasks.get(task_id)
            if task is None:
                return False

            if task.status == TaskStatus.PENDING:
                task.status = TaskStatus.CANCELLED
                self._stats['total_cancelled'] += 1
                logger.info(f"Cancelled pending transcode task {task_id}")
                return True

            if task.status == TaskStatus.RUNNING and task.process:
                self.process_manager.stop_process(task_id)
                task.status = TaskStatus.CANCELLED
                task.completed_at = time.time()
                self._stats['total_cancelled'] += 1
                logger.info(f"Cancelled running transcode task {task_id}")
                return True

            return False

    def get_stats(self) -> Dict:
        """Get manager statistics"""
        with self._lock:
            pending = sum(1 for t in self._tasks.values() if t.status == TaskStatus.PENDING)
            running = sum(1 for t in self._tasks.values() if t.status == TaskStatus.RUNNING)

            return {
                **self._stats,
                'pending': pending,
                'running': running,
                'max_concurrent': self.max_concurrent,
                'queue_size': self._task_queue.qsize(),
            }

    def _worker_loop(self):
        """Background worker that processes the task queue"""
        while not self._stop_event.is_set():
            try:
                # Try to get a task from queue with timeout
                try:
                    _, task_id = self._task_queue.get(timeout=1)
                except queue.Empty:
                    continue

                # Wait until we can start a process
                while not self.process_manager.can_start_process():
                    if self._stop_event.is_set():
                        break
                    time.sleep(0.5)

                if self._stop_event.is_set():
                    break

                # Start the task
                with self._lock:
                    task = self._tasks.get(task_id)
                    if task is None or task.status != TaskStatus.PENDING:
                        continue  # Task was cancelled or already started

                    # Create on_exit callback
                    def make_callback(tid):
                        def on_exit(pid, exit_code):
                            self._on_task_complete(tid, exit_code)
                        return on_exit

                    # Start process
                    process = self.process_manager.start_process(
                        task_id,
                        task.cmd,
                        on_exit=make_callback(task_id)
                    )

                    if process:
                        task.process = process
                        task.status = TaskStatus.RUNNING
                        task.started_at = time.time()
                        logger.info(f"Started transcode task {task_id} for stream {task.stream_id}")
                    else:
                        # Couldn't start, put back in queue
                        task.error_message = "Failed to start process"
                        task.status = TaskStatus.FAILED
                        task.completed_at = time.time()
                        self._stats['total_failed'] += 1
                        logger.error(f"Failed to start transcode task {task_id}")

                self._task_queue.task_done()

            except Exception as e:
                logger.error(f"Error in transcode worker loop: {e}", exc_info=True)
                time.sleep(1)

    def _on_task_complete(self, task_id: str, exit_code: int):
        """Callback when a task completes"""
        with self._lock:
            task = self._tasks.get(task_id)
            if task is None:
                return

            task.completed_at = time.time()

            if exit_code == 0:
                task.status = TaskStatus.COMPLETED
                self._stats['total_completed'] += 1
                logger.info(f"Transcode task {task_id} completed successfully")
            else:
                task.status = TaskStatus.FAILED
                task.error_message = f"FFmpeg exited with code {exit_code}"
                self._stats['total_failed'] += 1
                logger.error(f"Transcode task {task_id} failed with exit code {exit_code}")

    def cleanup_old_tasks(self, max_age_seconds: int = 3600):
        """Remove old completed/failed tasks to free memory"""
        cutoff = time.time() - max_age_seconds
        with self._lock:
            old_tasks = [
                tid for tid, task in self._tasks.items()
                if task.completed_at and task.completed_at < cutoff
                and task.status in (TaskStatus.COMPLETED, TaskStatus.FAILED, TaskStatus.CANCELLED)
            ]
            for tid in old_tasks:
                del self._tasks[tid]

        if old_tasks:
            logger.debug(f"Cleaned up {len(old_tasks)} old transcode tasks")

    def shutdown(self, wait_for_queue: bool = False):
        """Shutdown the task manager"""
        logger.info("Shutting down transcode task manager...")
        self._stop_event.set()

        if wait_for_queue:
            self._task_queue.join()

        # Cancel all running tasks
        with self._lock:
            for task_id, task in list(self._tasks.items()):
                if task.status == TaskStatus.RUNNING:
                    try:
                        self.process_manager.stop_process(task_id)
                    except Exception as e:
                        logger.error(f"Error stopping task {task_id}: {e}")

        self.process_manager.shutdown()
        logger.info("Transcode task manager shutdown complete")


# Global singleton instance
_task_manager: Optional[TranscodeTaskManager] = None


def get_task_manager(max_concurrent: Optional[int] = None,
                     queue_size: int = 100) -> TranscodeTaskManager:
    """Get or create the global transcode task manager"""
    global _task_manager
    if _task_manager is None:
        _task_manager = TranscodeTaskManager(max_concurrent=max_concurrent, queue_size=queue_size)
    return _task_manager
