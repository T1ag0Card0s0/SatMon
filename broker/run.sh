#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

compose() {
    if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
        docker compose "$@"
    elif command -v docker-compose >/dev/null 2>&1; then
        docker-compose "$@"
    else
        echo "Docker Compose is required. Install docker compose or docker-compose." >&2
        exit 1
    fi
}

validate_aws_iot_config() {
    if [[ "${SATMON_CLOUD_TARGET:-log}" != "aws_iot" ]]; then
        return
    fi

    if [[ -z "${SATMON_AWS_IOT_ENDPOINT:-}" ]]; then
        echo "SATMON_CLOUD_TARGET=aws_iot requires SATMON_AWS_IOT_ENDPOINT in broker/.env." >&2
        exit 1
    fi

    if command -v getent >/dev/null 2>&1 && ! getent hosts "$SATMON_AWS_IOT_ENDPOINT" >/dev/null; then
        echo "Cannot resolve SATMON_AWS_IOT_ENDPOINT=$SATMON_AWS_IOT_ENDPOINT." >&2
        echo "Set the current AWS IoT Core endpoint in broker/.env, or use SATMON_CLOUD_TARGET=log for local MQTT testing." >&2
        exit 1
    fi
}

if [[ ! -f .env ]]; then
    cp .env.example .env
    echo "Created broker/.env from .env.example."
fi

set -a
# shellcheck source=/dev/null
. .env
set +a

validate_aws_iot_config

mqtt_host_port="${SATMON_MQTT_HOST_PORT:-1883}"
websocket_host_port="${SATMON_WS_HOST_PORT:-9001}"

compose down --remove-orphans >/dev/null 2>&1 || true

if ss -H -ltn "sport = :$mqtt_host_port" | grep -q .; then
    echo "Port $mqtt_host_port is already in use. Stop the existing MQTT broker or set SATMON_MQTT_HOST_PORT in broker/.env." >&2
    exit 1
fi

if ss -H -ltn "sport = :$websocket_host_port" | grep -q .; then
    echo "Port $websocket_host_port is already in use. Stop the existing WebSocket service or set SATMON_WS_HOST_PORT in broker/.env." >&2
    exit 1
fi

compose up --build
