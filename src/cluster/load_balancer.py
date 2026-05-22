import logging
import random
import time
import aiohttp
import asyncio
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
    def __init__(self, nodes: List[str] = None):
        self.nodes = nodes or Config.CLUSTER_NODES
        self.node_status: Dict[str, NodeStatus] = {}
        self.check_interval = 30
        self.last_health_check = 0.0

    async def check_node_health(self, node: str) -> NodeStatus:
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
            logger.warning(f"Health check failed for {node}: {e}")
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
        healthy_nodes = [ns for ns in self.node_status.values() if ns.is_healthy]
        if not healthy_nodes:
            return None

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
        self.refresh_health_if_needed()
        healthy_nodes = [ns for ns in self.node_status.values() if ns.is_healthy]
        if not healthy_nodes and len(self.nodes) == 1:
            return self.nodes[0]
        if not healthy_nodes:
            return None

        return min(healthy_nodes, key=lambda x: x.active_connections).address

    def get_healthy_nodes(self) -> List[NodeStatus]:
        return [ns for ns in self.node_status.values() if ns.is_healthy]

    def refresh_health_if_needed(self):
        now = time.time()
        if now - self.last_health_check < self.check_interval and self.node_status:
            return

        try:
            asyncio.run(self.update_all_nodes_health())
            self.last_health_check = now
        except RuntimeError:
            if len(self.nodes) == 1 and self.nodes[0] not in self.node_status:
                self.node_status[self.nodes[0]] = NodeStatus(
                    address=self.nodes[0],
                    active_connections=0,
                    cpu_usage=0,
                    memory_usage=0,
                    bandwidth_usage=0,
                    is_healthy=True,
                    last_check=now
                )

    async def update_all_nodes_health(self):
        tasks = [self.check_node_health(node) for node in self.nodes]
        results = await asyncio.gather(*tasks, return_exceptions=True)

        for node, result in zip(self.nodes, results):
            if isinstance(result, Exception):
                if node in self.node_status:
                    self.node_status[node].is_healthy = False
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

        logger.info(f"Health check completed: {len(self.get_healthy_nodes())}/{len(self.nodes)} nodes healthy")


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
