# SatMon MQTT Broker Bridge

This package runs the local MQTT ingress path for ESP32 telemetry and the cloud downlink path for commands:

```text
ESP32 nodes -> local Mosquitto broker -> SatMon cloud bridge -> cloud
dashboard -> cloud API -> AWS IoT Core -> SatMon cloud bridge -> local Mosquitto broker -> ESP32 nodes
```

The bridge subscribes to `#`, so IoT nodes can publish on any local MQTT topic. It logs every local MQTT payload it receives, ignores internal status topics such as `satmon/bridge/#`, extracts `satellite_id` from node JSON payloads for logs, and forwards the node payload itself to the fixed AWS IoT topic `satmon/satellites`:

```json
{
  "satellite_id": "SAT-01",
  "timestamp": "2026-04-20T14:32:10Z",
  "sensors": {
    "mpu": {
      "accelerometer": {"x": 201, "y": -131, "z": 16410},
      "gyroscope": {"x": 4, "y": -2, "z": 0}
    },
    "temperature": 1251
  }
}
```

When `SATMON_CLOUD_TARGET=aws_iot`, the same bridge also subscribes to `SATMON_AWS_IOT_COMMAND_TOPIC`, which defaults to `satmon/commands/#`. Command messages from AWS IoT are republished to the local Mosquitto broker on the same topic, so the firmware receives dashboard commands at:

```text
satmon/commands/<satellite_id>/led
satmon/commands/all/led
```

The default `SATMON_LOCAL_MQTT_IGNORE_TOPICS` includes `satmon/commands/#`, which prevents command downlinks from being echoed back up to AWS IoT.

The AWS IoT policy attached to the bridge certificate must allow publish on the telemetry topic and subscribe/receive on `satmon/commands/*`.

## Run Locally

From the project root, run the broker and frontend dashboard together:

```bash
./run.sh
```

The dashboard is exposed at `http://localhost:5173` and the local MQTT broker is exposed on `localhost:1883`.

1. Start the broker and bridge:

```bash
cd broker
./run.sh
```

`run.sh` creates `.env` from `.env.example` the first time. The default target is `log`, so local MQTT traffic can be tested without a live AWS endpoint.

2. Set the cloud target in `.env` if you want a different mode:

- `SATMON_CLOUD_TARGET=log` only prints forwarded messages, useful before touching AWS.
- `SATMON_CLOUD_TARGET=aws_iot` publishes each message to AWS IoT Core using the cert bundle in `SATMON_AWS_CERT_DIR`.
- `SATMON_CLOUD_TARGET=http` posts each message to `SATMON_CLOUD_HTTP_URL`.

When `SATMON_CLOUD_TARGET=aws_iot`, `run.sh` checks that `SATMON_AWS_IOT_ENDPOINT` resolves before starting Compose. If the endpoint is stale or mistyped, it exits with a clear message instead of letting the bridge restart forever.

Every run streams Mosquitto broker logs plus bridge payload logs to the same terminal. The important bridge lines look like this:

```text
Local -> Bridge: Topic [satmon/mpu9265] (qos=1, retain=False). Msg "{...}"
Bridge -> AWS IoT: Topic [satmon/satellites] (qos=1, retain=False). Msg "{...}"
```

The ESP32 firmware should point `CONFIG_SATMON_MQTT_BROKER_URI` at the machine running this broker, for example `mqtt://192.168.1.77:1883`.

Mosquitto exposes two listeners:

- `1883`: MQTT TCP for ESP32 firmware and the Python bridge
- `9001`: MQTT over WebSockets for local diagnostics or custom tools

If either host port is already in use, set `SATMON_MQTT_HOST_PORT` or `SATMON_WS_HOST_PORT` in `.env` before running `./run.sh`.

## AWS Certs

The existing cert bundle is mounted into the bridge container at `/app/aws_certs`:

- `aws_certs/AmazonRootCA1.pem`
- `aws_certs/*-certificate.pem.crt`
- `aws_certs/*-private.pem.key`

For AWS IoT mode, `SATMON_AWS_IOT_ENDPOINT` must be the current account IoT endpoint, such as `xxxxxxxxxxxxx-ats.iot.eu-west-1.amazonaws.com`. The bridge can auto-discover `AmazonRootCA1.pem`, `*-certificate.pem.crt`, and `*-private.pem.key` inside `SATMON_AWS_CERT_DIR`; it also supports one nested cert folder if you later organize certificates per node.

## Node Onboarding

New ESP32 nodes only need to publish JSON containing a unique `satellite_id`. The bridge subscribes to all local topics with `#`, so adding `SAT-03` or changing the node publish topic does not require changing broker code.

For HTTP/API Gateway mode, the cloud endpoint should accept `POST` JSON bodies. If API Gateway requires an API key or authorization header, set `SATMON_CLOUD_HTTP_API_KEY` or `SATMON_CLOUD_HTTP_AUTHORIZATION` in `.env`.
