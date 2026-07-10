import logging
import random
import time
import aiohttp
import asyncio
import threading
from typing import List, Dict, Optional
from dataclasses import dataclass
from src.config import Config

logger = logging.getLogger(__name__)


@dataclass
class NodeStatus:
    address: str
    active_connections: int
    cpu_usage: float
    memory_usage: float
    bandwidth_usage: float
    is_healthy: bool
    last_check: float


class LoadBalancer:
    """
    Cluster Load Balancer with asynchronous background health checks.

    Health checks run in a background thread's event loop, avoiding:
    1. Blocking request handlers during health refresh
    2. asyncio.run() conflicts in existing event loop environments
    """

    def __init__(self, nodes: List[str] = None, check_interval: int = None):
        self.nodes = nodes or Config.CLUSTER_NODES
        self.node_status: Dict[str, NodeStatus] = {}
        self.check_interval = check_interval or Config.HEALTH_CHECK_INTERVAL or 30

        # Thread safety
        self._lock = threading.Lock()
        self._stop_event = threading.Event()
        self._health_thread: Optional[threading.Thread] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None

        # Initialize node status with unknown state
        self._init_node_status()

        # Start background health check thread
        if len(self.nodes) > 1:  # Only need LB when multiple nodes
            self._start_health_check_loop()
        else:
            logger.info(f"Single node mode ({self.nodes[0]}), skipping load balancer health checks")

    def _init_node_status(self):
        """Initialize all nodes as healthy initially"""
        now = time.time()
        for node in self.nodes:
            self.node_status[node] = NodeStatus(
                address=node,
                active_connections=0,
                cpu_usage=0,
                memory_usage=0,
                bandwidth_usage=0,
                is_healthy=True,
                last_check=now
            )

    def _start_health_check_loop(self):
        """Start the background health check thread with its own event loop"""

        def run_async_loop():
            """Thread function that runs the async event loop"""
            try:
                # Create a new event loop for this thread
                self._loop = asyncio.new_event_loop()
                asyncio.set_event_loop(self._loop)

                # Run the health check loop forever
                self._loop.run_until_complete(self._health_check_loop())
            except Exception as e:
                logger.error(f"Health check thread error: {e}", exc_info=True)
            finally:
                if self._loop:
                    self._loop.close()

        self._health_thread = threading.Thread(
            target=run_async_loop,
            daemon=True,
            name="lb-health-check"
        )
        self._health_thread.start()
        logger.info(f"Started background health check thread for {len(self.nodes)} nodes")

    async def _health_check_loop(self):
        """Async loop that periodically checks all nodes"""
        while not self._stop_event.is_set():
            try:
                await self.update_all_nodes_health()
            except Exception as e:
                logger.error(f"Error in health check loop: {e}", exc_info=True)

            # Wait for check interval or until stop
            try:
                await asyncio.wait_for(
                    asyncio.sleep(self.check_interval),
                    timeout=self.check_interval
                )
            except asyncio.TimeoutError:
                pass  # Expected, just means we waited full interval

    async def check_node_health(self, node: str) -> NodeStatus:
        """Check health of a single node asynchronously"""
        try:
            url = f"http://{node}:8000/api/health"
            async with aiohttp.ClientSession() as session:
                async with session.get(url, timeout=5) as response:
                    if response.status == 200:
                        data = await response.json()
                        return NodeStatus(
                            address=node,
                            active_connections=data.get('active_connections', 0),
                            cpu_usage=data.get('cpu_usage', 0),
                            memory_usage=data.get('memory_usage', 0),
                            bandwidth_usage=data.get('bandwidth_usage', 0),
                            is_healthy=True,
                            last_check=time.time()
                        )
                    else:
                        return NodeStatus(
                            address=node,
                            active_connections=0,
                            cpu_usage=100,
                            memory_usage=100,
                            bandwidth_usage=100,
                            is_healthy=False,
                            last_check=time.time()
                        )
        except Exception as e:
            logger.debug(f"Health check failed for {node}: {e}")
            return NodeStatus(
                address=node,
                active_connections=0,
                cpu_usage=100,
                memory_usage=100,
                bandwidth_usage=100,
                is_healthy=False,
                last_check=time.time()
            )

    def select_node_weighted(self, stream_id: str = None) -> Optional[str]:
        """Select node using weighted random based on CPU usage and connections"""
        with self._lock:
            healthy_nodes = [ns for ns in self.node_status.values() if ns.is_healthy]

        if not healthy_nodes:
            logger.warning("No healthy nodes available!")
            return None

        # Calculate weights: lower CPU + lower connections = higher weight
        weights = []
        for node in healthy_nodes:
            weight = max(1, 100 - int(node.cpu_usage) - int(node.active_connections / 100))
            weights.append(weight)

        total_weight = sum(weights)
        r = random.uniform(0, total_weight)
        cumulative = 0

        for node, weight in zip(healthy_nodes, weights):
            cumulative += weight
            if r < cumulative:
                return node.address

        return healthy_nodes[-1].address

    def select_node_least_conn(self) -> Optional[str]:
        """Select node with least active connections"""
        with self._lock:
            healthy_nodes = [ns for ns in self.node_status.values() if ns.is_healthy]

        if not healthy_nodes:
            logger.warning("No healthy nodes available!")
            return None

        return min(healthy_nodes, key=lambda x: x.active_connections).address

    def get_healthy_nodes(self) -> List[NodeStatus]:
        """Get list of all healthy nodes"""
        with self._lock:
            return [ns for ns in self.node_status.values() if ns.is_healthy]

    def get_all_node_status(self) -> Dict[str, NodeStatus]:
        """Get status of all nodes (thread-safe)"""
        with self._lock:
            return dict(self.node_status)

    async def update_all_nodes_health(self):
        """Update health for all nodes concurrently"""
        tasks = [self.check_node_health(node) for node in self.nodes]
        results = await asyncio.gather(*tasks, return_exceptions=True)

        with self._lock:
            for node, result in zip(self.nodes, results):
                if isinstance(result, Exception):
                    # Mark as unhealthy if already in status, otherwise create
                    if node in self.node_status:
                        self.node_status[node].is_healthy = False
                        self.node_status[node].last_check = time.time()
                    else:
                        self.node_status[node] = NodeStatus(
                            address=node,
                            active_connections=0,
                            cpu_usage=100,
                            memory_usage=100,
                            bandwidth_usage=100,
                            is_healthy=False,
                            last_check=time.time()
                        )
                else:
                    self.node_status[node] = result

        healthy_count = len(self.get_healthy_nodes())
        logger.debug(f"Health check: {healthy_count}/{len(self.nodes)} nodes healthy")

    def shutdown(self):
        """Gracefully shutdown the background health checker"""
        logger.info("Shutting down load balancer health checker...")
        self._stop_event.set()

        if self._health_thread and self._health_thread.is_alive():
            self._health_thread.join(timeout=5)

        logger.info("Load balancer shutdown complete")


class CDNDistributor:
    def __init__(self):
        self.multicast_enabled = True
        self.multicast_groups: Dict[str, str] = {}

    def should_use_multicast(self, connections: int) -> bool:
        return self.multicast_enabled and connections >= 10

    def get_multicast_url(self, stream_id: str) -> Optional[str]:
        return self.multicast_groups.get(stream_id)

    def register_multicast_stream(self, stream_id: str, group_address: str):
        self.multicast_groups[stream_id] = group_address
        logger.info(f"Registered multicast stream {stream_id} at {group_address}")

    def unregister_multicast_stream(self, stream_id: str):
        if stream_id in self.multicast_groups:
            del self.multicast_groups[stream_id]
            logger.info(f"Unregistered multicast stream {stream_id}")
