from __future__ import annotations

import json
import logging
import os
import ssl
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib import error, request

import paho.mqtt.client as mqtt


LOG = logging.getLogger("satmon.cloud_bridge")


def env(name: str, default: str = "") -> str:
    return os.getenv(name, default).strip()


def env_int(name: str, default: int) -> int:
    value = env(name)
    if not value:
        return default
    return int(value)


def env_bool(name: str, default: bool = False) -> bool:
    value = env(name)
    if not value:
        return default
    return value.lower() in {"1", "true", "yes", "on"}


def make_mqtt_client(client_id: str) -> mqtt.Client:
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    except (AttributeError, TypeError):
        return mqtt.Client(client_id=client_id)


def mqtt_reason_ok(reason_code: Any) -> bool:
    try:
        return int(reason_code) == 0
    except (TypeError, ValueError):
        return str(reason_code).lower() in {"0", "success"}


def topic_node_hint(topic: str) -> str:
    parts = [part for part in topic.split("/") if part]
    if len(parts) >= 2 and parts[0].lower() == "satmon":
        return parts[1]
    return parts[-1] if parts else "unknown"


def decode_payload(raw_payload: bytes) -> tuple[dict[str, Any] | None, str | None]:
    text = raw_payload.decode("utf-8", errors="replace")
    try:
        decoded = json.loads(text)
    except json.JSONDecodeError:
        return None, text
    if isinstance(decoded, dict):
        return decoded, None
    return {"value": decoded}, None


def payload_log_text(payload: bytes | str) -> str:
    if isinstance(payload, bytes):
        return payload.decode("utf-8", errors="replace")
    return payload


def dump_json(message: dict[str, Any]) -> str:
    return json.dumps(message, separators=(",", ":"))


def log_mqtt_payload(direction: str, topic: str, payload: bytes | str, qos: int | None = None, retain: bool | None = None) -> None:
    details = []
    if qos is not None:
        details.append(f"qos={qos}")
    if retain is not None:
        details.append(f"retain={retain}")
    detail_text = f" ({', '.join(details)})" if details else ""
    LOG.info('%s: Topic [%s]%s. Msg "%s"', direction, topic, detail_text, payload_log_text(payload))


def mqtt_filter_matches(topic_filter: str, topic: str) -> bool:
    filter_levels = topic_filter.split("/")
    topic_levels = topic.split("/")

    for index, filter_level in enumerate(filter_levels):
        if filter_level == "#":
            return index == len(filter_levels) - 1
        if index >= len(topic_levels):
            return False
        if filter_level != "+" and filter_level != topic_levels[index]:
            return False

    return len(topic_levels) == len(filter_levels)


def parse_topic_filters(value: str) -> list[str]:
    return [topic_filter.strip() for topic_filter in value.split(",") if topic_filter.strip()]


def build_cloud_message(topic: str, payload: bytes) -> tuple[str, dict[str, Any]]:
    decoded, raw_text = decode_payload(payload)
    node_id = topic_node_hint(topic)

    if decoded is not None:
        node_id = str(decoded.get("satellite_id") or decoded.get("node_id") or node_id)
        return node_id, decoded

    return node_id, {"raw_payload": raw_text}


class CloudPublisher:
    def publish(self, node_id: str, message: dict[str, Any]) -> None:
        raise NotImplementedError

    def close(self) -> None:
        pass


class LogPublisher(CloudPublisher):
    def publish(self, node_id: str, message: dict[str, Any]) -> None:
        LOG.info("Bridge -> cloud log node=%s payload=%s", node_id, dump_json(message))


@dataclass(frozen=True)
class HttpConfig:
    url: str
    timeout_seconds: int
    api_key: str
    authorization: str


class HttpPublisher(CloudPublisher):
    def __init__(self, config: HttpConfig) -> None:
        if not config.url:
            raise ValueError("SATMON_CLOUD_HTTP_URL is required when SATMON_CLOUD_TARGET=http")
        self._config = config

    def publish(self, node_id: str, message: dict[str, Any]) -> None:
        payload = dump_json(message)
        LOG.info("Bridge -> HTTP cloud node=%s url=%s payload=%s", node_id, self._config.url, payload)
        body = payload.encode("utf-8")
        headers = {"Content-Type": "application/json"}
        if self._config.api_key:
            headers["x-api-key"] = self._config.api_key
        if self._config.authorization:
            headers["Authorization"] = self._config.authorization

        http_request = request.Request(self._config.url, data=body, headers=headers, method="POST")
        try:
            with request.urlopen(http_request, timeout=self._config.timeout_seconds) as response:
                LOG.info("Forwarded node=%s to HTTP cloud status=%s", node_id, response.status)
        except error.HTTPError as exc:
            response_body = exc.read().decode("utf-8", errors="replace")
            LOG.error("HTTP cloud rejected node=%s status=%s body=%s", node_id, exc.code, response_body)
            raise
        except error.URLError as exc:
            LOG.error("HTTP cloud request failed for node=%s reason=%s", node_id, exc.reason)
            raise


@dataclass(frozen=True)
class AwsIotConfig:
    endpoint: str
    port: int
    client_id: str
    topic: str
    command_topic_filter: str
    cert_dir: Path
    ca_file: Path | None
    cert_file: Path | None
    key_file: Path | None


def first_match(directory: Path, pattern: str) -> Path | None:
    matches = sorted(directory.glob(pattern))
    if not matches:
        matches = sorted(directory.glob(f"*/{pattern}"))
    return matches[0] if matches else None


class AwsIotPublisher(CloudPublisher):
    def __init__(self, config: AwsIotConfig) -> None:
        if not config.endpoint:
            raise ValueError("SATMON_AWS_IOT_ENDPOINT is required when SATMON_CLOUD_TARGET=aws_iot")

        ca_file = config.ca_file or first_match(config.cert_dir, "AmazonRootCA1.pem")
        cert_file = config.cert_file or first_match(config.cert_dir, "*-certificate.pem.crt")
        key_file = config.key_file or first_match(config.cert_dir, "*-private.pem.key")
        missing = [
            name
            for name, path in (("CA", ca_file), ("certificate", cert_file), ("private key", key_file))
            if path is None
        ]
        if missing:
            raise ValueError(f"Missing AWS IoT files in {config.cert_dir}: {', '.join(missing)}")

        LOG.info("Using AWS IoT certs ca=%s cert=%s key=%s", ca_file, cert_file, key_file)

        self._topic = config.topic.strip("/")
        if not self._topic:
            raise ValueError("SATMON_AWS_IOT_TOPIC must not be empty")
        self._command_topic_filter = config.command_topic_filter.strip() or "satmon/commands/#"
        self._command_handler: Any = None
        self._connected = threading.Event()
        self._client = make_mqtt_client(config.client_id)
        if env_bool("SATMON_MQTT_CLIENT_LOGS"):
            self._client.enable_logger(logging.getLogger("satmon.mqtt.aws_iot"))
        self._client.tls_set(
            ca_certs=str(ca_file),
            certfile=str(cert_file),
            keyfile=str(key_file),
            tls_version=ssl.PROTOCOL_TLSv1_2,
        )
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message
        self._client.reconnect_delay_set(min_delay=1, max_delay=30)
        LOG.info("Connecting to AWS IoT MQTT at %s:%s", config.endpoint, config.port)
        self._client.connect(config.endpoint, config.port, keepalive=60)
        self._client.loop_start()

        if not self._connected.wait(timeout=15):
            raise TimeoutError("Timed out connecting to AWS IoT MQTT")

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Any, reason_code: Any, properties: Any = None) -> None:
        if mqtt_reason_ok(reason_code):
            LOG.info("Connected to AWS IoT MQTT")
            self._connected.set()
            if self._command_topic_filter:
                LOG.info("Subscribing to AWS IoT command topic=%s", self._command_topic_filter)
                client.subscribe(self._command_topic_filter, qos=1)
        else:
            LOG.error("AWS IoT MQTT connection failed reason=%s", reason_code)

    def _on_disconnect(self, client: mqtt.Client, userdata: Any, flags: Any, reason_code: Any, properties: Any = None) -> None:
        LOG.warning("Disconnected from AWS IoT MQTT reason=%s", reason_code)
        self._connected.clear()

    def _on_message(self, client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        log_mqtt_payload("AWS IoT -> Bridge", message.topic, message.payload, qos=message.qos, retain=message.retain)
        if self._command_handler is None:
            LOG.warning("No local command handler configured for AWS IoT topic=%s", message.topic)
            return

        self._command_handler(message.topic, message.payload, message.qos, message.retain)

    def set_command_handler(self, handler: Any) -> None:
        self._command_handler = handler

    def publish(self, node_id: str, message: dict[str, Any]) -> None:
        payload = dump_json(message)
        log_mqtt_payload("Bridge -> AWS IoT", self._topic, payload, qos=1, retain=False)
        info = self._client.publish(self._topic, payload=payload, qos=1, retain=False)
        info.wait_for_publish(timeout=10)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError(f"AWS IoT publish failed rc={info.rc}")
        LOG.info("AWS IoT publish acknowledged node=%s topic=%s mid=%s", node_id, self._topic, info.mid)

    def close(self) -> None:
        self._client.loop_stop()
        self._client.disconnect()


def build_publisher() -> CloudPublisher:
    target = env("SATMON_CLOUD_TARGET", "log").lower()
    if target == "log":
        return LogPublisher()
    if target == "http":
        return HttpPublisher(
            HttpConfig(
                url=env("SATMON_CLOUD_HTTP_URL"),
                timeout_seconds=env_int("SATMON_CLOUD_HTTP_TIMEOUT_SECONDS", 10),
                api_key=env("SATMON_CLOUD_HTTP_API_KEY"),
                authorization=env("SATMON_CLOUD_HTTP_AUTHORIZATION"),
            )
        )
    if target == "aws_iot":
        return AwsIotPublisher(
            AwsIotConfig(
                endpoint=env("SATMON_AWS_IOT_ENDPOINT"),
                port=env_int("SATMON_AWS_IOT_PORT", 8883),
                client_id=env("SATMON_AWS_IOT_CLIENT_ID", "satmon-broker-bridge"),
                topic=env("SATMON_AWS_IOT_TOPIC", env("SATMON_AWS_IOT_TOPIC_PREFIX", "satmon/satellites")),
                command_topic_filter=env("SATMON_AWS_IOT_COMMAND_TOPIC", "satmon/commands/#"),
                cert_dir=Path(env("SATMON_AWS_CERT_DIR", "/app/aws_certs")),
                ca_file=Path(env("SATMON_AWS_IOT_CA_FILE")) if env("SATMON_AWS_IOT_CA_FILE") else None,
                cert_file=Path(env("SATMON_AWS_IOT_CERT_FILE")) if env("SATMON_AWS_IOT_CERT_FILE") else None,
                key_file=Path(env("SATMON_AWS_IOT_KEY_FILE")) if env("SATMON_AWS_IOT_KEY_FILE") else None,
            )
        )
    raise ValueError(f"Unsupported SATMON_CLOUD_TARGET={target!r}")


def main() -> None:
    logging.basicConfig(
        level=env("SATMON_LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(message)s",
        stream=sys.stdout,
    )

    topic_filter = env("SATMON_LOCAL_MQTT_TOPIC", "#")
    ignore_filters = parse_topic_filters(env("SATMON_LOCAL_MQTT_IGNORE_TOPICS", "satmon/bridge/#,satmon/commands/#"))
    local_host = env("SATMON_LOCAL_MQTT_HOST", "localhost")
    local_port = env_int("SATMON_LOCAL_MQTT_PORT", 1883)
    LOG.info(
        "Starting SatMon bridge local=%s:%s topic=%s ignore=%s target=%s",
        local_host,
        local_port,
        topic_filter,
        ignore_filters or "none",
        env("SATMON_CLOUD_TARGET", "log"),
    )
    publisher = build_publisher()
    seen_nodes: set[str] = set()

    def on_connect(client: mqtt.Client, userdata: Any, flags: Any, reason_code: Any, properties: Any = None) -> None:
        if not mqtt_reason_ok(reason_code):
            LOG.error("Local MQTT connection failed reason=%s", reason_code)
            return
        LOG.info("Connected to local MQTT broker, subscribing to %s", topic_filter)
        client.subscribe(topic_filter, qos=1)

    def on_subscribe(client: mqtt.Client, userdata: Any, mid: int, reason_codes: Any = None, properties: Any = None) -> None:
        LOG.info("Subscribed to local MQTT topic=%s mid=%s reason=%s", topic_filter, mid, reason_codes)

    def on_disconnect(client: mqtt.Client, userdata: Any, *args: Any) -> None:
        reason_code = args[1] if len(args) >= 2 else args[0] if args else "unknown"
        LOG.warning("Disconnected from local MQTT broker reason=%s", reason_code)

    def on_log(client: mqtt.Client, userdata: Any, level: int, buffer: str) -> None:
        LOG.debug("Local MQTT client: %s", buffer)

    def on_message(client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        try:
            log_mqtt_payload("Local -> Bridge", message.topic, message.payload, qos=message.qos, retain=message.retain)
            if any(mqtt_filter_matches(ignored_topic, message.topic) for ignored_topic in ignore_filters):
                LOG.info("Ignored local MQTT topic=%s", message.topic)
                return
            node_id, cloud_message = build_cloud_message(message.topic, message.payload)
            if node_id not in seen_nodes:
                seen_nodes.add(node_id)
                LOG.info("Discovered node=%s from topic=%s", node_id, message.topic)
            publisher.publish(node_id, cloud_message)
        except Exception:
            LOG.exception("Failed to forward message from topic=%s", message.topic)

    def publish_cloud_command(topic: str, payload: bytes, qos: int, retain: bool) -> None:
        try:
            log_mqtt_payload("Bridge -> Local", topic, payload, qos=1, retain=False)
            info = client.publish(topic, payload=payload, qos=1, retain=False)
            info.wait_for_publish(timeout=10)
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                raise RuntimeError(f"Local MQTT command publish failed rc={info.rc}")
            LOG.info("Forwarded cloud command to local MQTT topic=%s mid=%s", topic, info.mid)
        except Exception:
            LOG.exception("Failed to publish cloud command topic=%s", topic)

    client = make_mqtt_client(env("SATMON_LOCAL_MQTT_CLIENT_ID", "satmon-cloud-bridge"))
    if env_bool("SATMON_MQTT_CLIENT_LOGS"):
        client.enable_logger(logging.getLogger("satmon.mqtt.local"))
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_subscribe = on_subscribe
    client.on_message = on_message
    client.on_log = on_log
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    LOG.info("Connecting to local MQTT broker at %s:%s", local_host, local_port)
    client.connect(local_host, local_port, keepalive=60)
    if isinstance(publisher, AwsIotPublisher):
        publisher.set_command_handler(publish_cloud_command)

    try:
        client.loop_forever()
    finally:
        publisher.close()


if __name__ == "__main__":
    main()
