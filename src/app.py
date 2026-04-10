import os
import logging
import psutil
from flask import Flask, Flask, request, jsonify, send_file, redirect
from flask_cors import CORS
from prometheus_client import generate_latest, Counter, Gauge, Histogram, CONTENT_TYPE_LATEST

from src.config import Config
from src.auth.jwt_auth import JWTAuthenticator
from src.streaming.profile_manager import ProfileManager
from src.streaming.hls_generator import HLSGenerator, HLSPlayListChecker
from src.transcoder.ffmpeg import FFmpegTranscoder, BitrateAdaptor
from src.cluster.load_balancer import LoadBalancer
from src.timeshift.recorder import TimeshiftRecorder

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

app = Flask(__name__)
CORS(app)

TRANSCODE_CONFIG_PATH = os.environ.get(
    'TRANSCODE_CONFIG_PATH',
    './config/transcode/profiles.json'
)

authenticator = JWTAuthenticator()
profile_manager = ProfileManager(TRANSCODE_CONFIG_PATH)
hls_generator = HLSGenerator(
    base_path=Config.HLS_BASE_PATH,
    fragment_duration=Config.HLS_FRAGMENT_DURATION,
    window_size=Config.HLS_WINDOW_SIZE
)
transcoder = FFmpegTranscoder()
bitrate_adaptor = BitrateAdaptor()
load_balancer = LoadBalancer()
timeshift_recorder = TimeshiftRecorder()
playlist_checker = HLSPlayListChecker(Config.HLS_BASE_PATH)

REQUEST_COUNT = Counter('iptv_requests_total', 'Total requests', ['method', 'endpoint'])
ACTIVE_CONNECTIONS = Gauge('iptv_active_connections', 'Number of active connections')
REQUEST_DURATION = Histogram('iptv_request_duration_seconds', 'Request duration')
STREAM_BANDWIDTH = Gauge('iptv_stream_bandwidth_bits', 'Stream bandwidth in bits per second', ['stream'])


@app.route('/api/stream/<stream_id>/playlist.m3u8')
@authenticator.auth_required
def get_master_playlist(stream_id):
    REQUEST_COUNT.labels(method=request.method, endpoint='/api/stream/playlist').inc()

    token = request.args.get('token')
    if Config.ENABLE_AUTH:
        client_ip = request.remote_addr
        if not token or not authenticator.verify_m3u8_token(token, stream_id, client_ip):
            return jsonify({'error': 'Invalid or missing token'}), 403

    profiles = [p.to_dict() for p in profile_manager.get_all_profiles()]
    content = hls_generator.generate_master_playlist(stream_id, profiles)

    return content, 200, {'Content-Type': 'application/vnd.apple.mpegurl'}


@app.route('/api/stream/<stream_id>/<profile>.m3u8')
@authenticator.auth_required
def get_media_playlist(stream_id, profile):
    REQUEST_COUNT.labels(method=request.method, endpoint='/api/stream/media').inc()

    playlist_path = hls_generator.get_playlist_path(stream_id, profile)

    if not os.path.exists(playlist_path):
        return jsonify({'error': 'Playlist not found'}), 404

    check_result = playlist_checker.check_integrity(playlist_path)
    if not check_result['valid']:
        logger.warning(f"Playlist {playlist_path} has issues: {check_result}")

    with open(playlist_path, 'r', encoding='utf-8') as f:
        content = f.read()

    return content, 200, {'Content-Type': 'application/vnd.apple.mpegurl'}


@app.route('/api/stream/<stream_id>/<ts_file>')
@authenticator.auth_required
def get_ts_segment(stream_id, ts_file):
    REQUEST_COUNT.labels(method=request.method, endpoint='/api/stream/ts').inc()

    ts_path = os.path.join(Config.HLS_BASE_PATH, stream_id, ts_file)

    if not os.path.exists(ts_path):
        return jsonify({'error': 'Segment not found'}), 404

    if not playlist_checker.verify_ts_file(ts_path):
        return jsonify({'error': 'Corrupted segment'}), 500

    return send_file(ts_path, mimetype='video/MP2T')


@app.route('/api/vod/<vod_id>')
@authenticator.auth_required
def get_vod_playlist(vod_id):
    REQUEST_COUNT.labels(method=request.method, endpoint='/api/vod').inc()

    vod_path = os.path.join(Config.VOD_BASE_PATH, f"{vod_id}.m3u8")
    if not os.path.exists(vod_path):
        return jsonify({'error': 'VOD not found'}), 404

    with open(vod_path, 'r', encoding='utf-8') as f:
        content = f.read()

    return content, 200, {'Content-Type': 'application/vnd.apple.mpegurl'}


@app.route('/api/timeshift/<channel_id>')
@authenticator.auth_required
def get_timeshift(channel_id):
    REQUEST_COUNT.labels(method=request.method, endpoint='/api/timeshift').inc()

    start_time = request.args.get('start')
    duration = request.args.get('duration', '3600')

    if not start_time:
        return jsonify({'error': 'Missing start time'}), 400

    try:
        from datetime import datetime
        start_dt = datetime.fromtimestamp(int(start_time))
        duration = int(duration)

        segment_path = timeshift_recorder.get_record_segment(channel_id, start_dt, duration)
        if not segment_path:
            return jsonify({'error': 'Timeshift segment not found'}), 404

        return send_file(segment_path, mimetype='video/MP2T')
    except Exception as e:
        logger.error(f"Timeshift error: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/api/recommend-profile', methods=['POST'])
def recommend_profile():
    data = request.get_json()
    client_id = request.remote_addr
    bandwidth = data.get('bandwidth', 0)

    bitrate_adaptor.update_bandwidth(client_id, bandwidth)
    recommended = bitrate_adaptor.recommend_profile(
        client_id,
        profile_manager.get_profile_names()
    )

    profile = profile_manager.get_profile(recommended)
    return jsonify({
        'recommended_profile': recommended,
        'profile_details': profile.to_dict() if profile else None
    })


@app.route('/api/cluster/select-node/<stream_id>')
def select_node(stream_id):
    node = load_balancer.select_node_least_conn()
    if not node:
        return jsonify({'error': 'No healthy nodes available'}), 503

    return jsonify({
        'selected_node': node,
        'stream_id': stream_id
    })


@app.route('/api/health')
def health():
    cpu_percent = psutil.cpu_percent()
    memory = psutil.virtual_memory()

    active_conns = ACTIVE_CONNECTIONS._value.get() if hasattr(ACTIVE_CONNECTIONS, '_value') else 0

    return jsonify({
        'status': 'healthy',
        'cpu_usage': cpu_percent,
        'memory_usage': memory.percent,
        'active_connections': int(active_conns),
        'timestamp': None
    })


@app.route('/metrics')
def metrics():
    return generate_latest(), 200, {'Content-Type': CONTENT_TYPE_LATEST}


@app.route('/api/profiles')
def list_profiles():
    profiles = [p.to_dict() for p in profile_manager.get_all_profiles()]
    return jsonify({'profiles': profiles})


@app.before_request
def before_request():
    ACTIVE_CONNECTIONS.inc()


@app.after_request
def after_request(response):
    ACTIVE_CONNECTIONS.dec()
    return response


def init_app():
    logger.info("Initializing IPTV backend server")
    logger.info(f"Loaded {len(profile_manager.get_all_profiles())} transcode profiles")
    logger.info(f"Cluster nodes: {Config.CLUSTER_NODES}")
    return app


if __name__ == '__main__':
    app = init_app()
    port = int(os.environ.get('PORT', '8000'))
    debug = os.environ.get('DEBUG', 'false').lower() == 'true'
    app.run(host='0.0.0.0', port=port, debug=debug)
