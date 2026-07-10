import pytest
import sys
import os

# Add src directory to Python path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '../src')))


@pytest.fixture
def sample_config():
    """Sample configuration fixture"""
    return {
        "jwt_secret": "test-secret-key-12345",
        "hls_base_path": "/tmp/test-hls",
    }


@pytest.fixture
def temp_dir(tmp_path):
    """Temporary directory fixture"""
    return str(tmp_path)
