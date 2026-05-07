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

port_in_use() {
    local port="$1"

    if command -v ss >/dev/null 2>&1; then
        ss -H -ltn "sport = :$port" | grep -q .
        return
    fi

    if command -v lsof >/dev/null 2>&1; then
        lsof -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1
        return
    fi

    return 1
}

check_port() {
    local port="$1"
    local label="$2"
    local env_name="$3"

    if port_in_use "$port"; then
        echo "Port $port is already in use by another process ($label)." >&2
        echo "Stop that process or set $env_name in broker/.env before running ./run.sh." >&2
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

if [[ ! -f broker/.env ]]; then
    cp broker/.env.example broker/.env
    echo "Created broker/.env from broker/.env.example."
fi

set -a
# shellcheck source=/dev/null
. broker/.env
set +a

validate_aws_iot_config

mqtt_host_port="${SATMON_MQTT_HOST_PORT:-1883}"
websocket_host_port="${SATMON_WS_HOST_PORT:-9001}"
frontend_host_port="${SATMON_FRONTEND_PORT:-5173}"

compose down --remove-orphans >/dev/null 2>&1 || true

if [[ -f broker/docker-compose.yml ]]; then
    (cd broker && compose down --remove-orphans >/dev/null 2>&1 || true)
fi

check_port "$mqtt_host_port" "MQTT broker" "SATMON_MQTT_HOST_PORT"
check_port "$websocket_host_port" "MQTT WebSocket" "SATMON_WS_HOST_PORT"
check_port "$frontend_host_port" "SatMon frontend" "SATMON_FRONTEND_PORT"

echo "Starting SatMon stack"
echo "  MQTT:      localhost:${mqtt_host_port}"
echo "  MQTT WS:   ws://localhost:${websocket_host_port}"
echo "  Frontend:  http://localhost:${frontend_host_port}"
echo "  API proxy: ${SATMON_API_TARGET:-https://n06cy09ved.execute-api.eu-west-1.amazonaws.com/dev/SatMonHTTP}"

compose up --build