import base64
import datetime as dt
import json
import logging
import os
from decimal import Decimal
from urllib.parse import unquote

import boto3
from boto3.dynamodb.conditions import Key
from botocore.exceptions import ClientError


TABLE_NAME = os.environ.get("TABLE_NAME", "SatMonDB")
AWS_REGION = os.environ.get("AWS_REGION", "eu-west-1")
IOT_DATA_ENDPOINT = os.environ.get("AWS_IOT_DATA_ENDPOINT") or os.environ.get("SATMON_AWS_IOT_ENDPOINT", "")
COMMAND_TOPIC_PREFIX = os.environ.get("SATMON_COMMAND_TOPIC_PREFIX", "satmon/commands").strip("/")
MAX_LIMIT = 500
ROUTE_PREFIXES = ("/SatMonHTTP", "/myFunctionName")

logger = logging.getLogger()
logger.setLevel(logging.INFO)

dynamodb = boto3.resource("dynamodb")
table = dynamodb.Table(TABLE_NAME)
iot_data = None


class AwsIotPublishError(RuntimeError):
    pass


def client_error_details(error):
    error_body = (error.response or {}).get("Error") or {}
    code = error_body.get("Code") or error.__class__.__name__
    message = error_body.get("Message") or str(error)
    return code, message


def json_default(value):
    if isinstance(value, Decimal):
        if value % 1 == 0:
            return int(value)
        return float(value)

    raise TypeError(f"Object of type {type(value).__name__} is not JSON serializable")


def response(status_code, body):
    return {
        "statusCode": status_code,
        "headers": {
            "Content-Type": "application/json",
            "Access-Control-Allow-Origin": "*",
            "Access-Control-Allow-Headers": "content-type,accept",
            "Access-Control-Allow-Methods": "GET,POST,OPTIONS",
        },
        "body": json.dumps(body, default=json_default),
    }


def get_iot_data_client():
    global iot_data

    if iot_data is not None:
        return iot_data

    client_kwargs = {"region_name": AWS_REGION}
    if IOT_DATA_ENDPOINT:
        endpoint_url = IOT_DATA_ENDPOINT
        if not endpoint_url.startswith(("http://", "https://")):
            endpoint_url = f"https://{endpoint_url}"
        client_kwargs["endpoint_url"] = endpoint_url

    iot_data = boto3.client("iot-data", **client_kwargs)
    return iot_data


def parse_json_body(event):
    raw_body = event.get("body")

    if raw_body is None or raw_body == "":
        return {}

    if isinstance(raw_body, dict):
        return raw_body

    if event.get("isBase64Encoded"):
        raw_body = base64.b64decode(raw_body).decode("utf-8")

    body = json.loads(raw_body)
    if not isinstance(body, dict):
        raise ValueError("Request body must be a JSON object")

    return body


def normalize_hex_color(value):
    color = str(value or "").strip()
    if color.startswith("#"):
        color = color[1:]

    if len(color) != 6 or any(character not in "0123456789abcdefABCDEF" for character in color):
        raise ValueError("color must be a hex value like #14c3b7")

    return f"#{color.lower()}"


def color_to_rgb(color):
    color = color.lstrip("#")
    return int(color[0:2], 16), int(color[2:4], 16), int(color[4:6], 16)


def rgb_component(body, key):
    value = body.get(key)
    if value is None:
        return None

    try:
        numeric = int(value)
    except (TypeError, ValueError):
        raise ValueError(f"{key} must be an integer between 0 and 255")

    if numeric < 0 or numeric > 255:
        raise ValueError(f"{key} must be an integer between 0 and 255")

    return numeric


def led_command_payload(satellite_id, body):
    enabled = body.get("enabled")
    if not isinstance(enabled, bool):
        raise ValueError("enabled must be true or false")

    if "color" in body:
        color = normalize_hex_color(body.get("color"))
        red, green, blue = color_to_rgb(color)
    else:
        red = rgb_component(body, "red")
        green = rgb_component(body, "green")
        blue = rgb_component(body, "blue")

        if red is None or green is None or blue is None:
            raise ValueError("Provide either color or red, green, and blue")

        color = f"#{red:02x}{green:02x}{blue:02x}"

    return {
        "satellite_id": satellite_id,
        "command": "led",
        "enabled": enabled,
        "color": color,
        "red": red,
        "green": green,
        "blue": blue,
        "timestamp": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": "satmon-http-api",
    }


def publish_led_command(satellite_id, body):
    if not satellite_id:
        raise ValueError("Missing satellite_id")

    payload = led_command_payload(satellite_id, body)
    topic = f"{COMMAND_TOPIC_PREFIX}/{satellite_id}/led"
    try:
        get_iot_data_client().publish(
            topic=topic,
            qos=1,
            payload=json.dumps(payload, separators=(",", ":")).encode("utf-8"),
        )
    except ClientError as error:
        code, message = client_error_details(error)
        logger.exception(
            "AWS IoT LED command publish failed satellite_id=%s topic=%s code=%s message=%s",
            satellite_id,
            topic,
            code,
            message,
        )
        raise AwsIotPublishError(f"{code}: {message}") from error

    logger.info("Published LED command satellite_id=%s topic=%s payload=%s", satellite_id, topic, payload)

    return {
        "ok": True,
        "topic": topic,
        "payload": payload,
    }


def parse_event(event):
    request_context = event.get("requestContext") or {}
    path_parameters = event.get("pathParameters") or {}

    if "http" in request_context:
        method = request_context["http"].get("method", "GET")
    else:
        method = event.get("httpMethod", "GET")

    proxy_path = path_parameters.get("proxy")

    if proxy_path:
        path = "/" + proxy_path.lstrip("/")
    else:
        if "http" in request_context:
            path = event.get("rawPath") or request_context["http"].get("path") or "/"
        else:
            path = event.get("path") or "/"

        stage = request_context.get("stage")
        if stage and path.startswith(f"/{stage}/"):
            path = path[len(stage) + 1:]

        for route_prefix in ROUTE_PREFIXES:
            if path == route_prefix:
                path = "/health"
                break

            if path.startswith(route_prefix + "/"):
                path = path[len(route_prefix):]
                break

    query = event.get("queryStringParameters") or {}
    path_parts = [unquote(part) for part in path.strip("/").split("/") if part]

    logger.info(
        "Parsed request method=%s rawPath=%s httpPath=%s proxy=%s path=%s path_parts=%s query=%s routeKey=%s",
        method,
        event.get("rawPath"),
        (request_context.get("http") or {}).get("path"),
        proxy_path,
        path,
        path_parts,
        query,
        request_context.get("routeKey"),
    )

    return method.upper(), path_parts, query


def parse_limit(query):
    raw_limit = query.get("limit", "100")

    try:
        limit = int(raw_limit)
    except ValueError:
        limit = 100

    return max(1, min(limit, MAX_LIMIT))


def scan_all_items():
    items = []
    scan_kwargs = {}

    while True:
        result = table.scan(**scan_kwargs)
        items.extend(result.get("Items", []))

        last_key = result.get("LastEvaluatedKey")
        if not last_key:
            break

        scan_kwargs["ExclusiveStartKey"] = last_key

    return items


def list_satellites():
    items = scan_all_items()
    satellites = {}

    for item in items:
        satellite_id = item.get("satellite_id")
        timestamp = item.get("timestamp")

        if not satellite_id:
            continue

        current = satellites.get(satellite_id)

        if current is None:
            satellites[satellite_id] = {
                "satellite_id": satellite_id,
                "latest_timestamp": timestamp,
                "messages": 1,
            }
        else:
            current["messages"] += 1
            if timestamp and (
                current["latest_timestamp"] is None
                or timestamp > current["latest_timestamp"]
            ):
                current["latest_timestamp"] = timestamp

    return {
        "satellites": sorted(
            satellites.values(),
            key=lambda satellite: satellite["satellite_id"],
        )
    }


def get_latest_telemetry(satellite_id):
    result = table.query(
        KeyConditionExpression=Key("satellite_id").eq(satellite_id),
        ScanIndexForward=False,
        Limit=1,
    )

    items = result.get("Items", [])

    if not items:
        return None

    return items[0]


def get_telemetry(satellite_id, query):
    limit = parse_limit(query)
    ascending = query.get("order", "desc").lower() == "asc"
    key_expression = Key("satellite_id").eq(satellite_id)
    start_timestamp = query.get("from")
    end_timestamp = query.get("to")

    if start_timestamp and end_timestamp:
        key_expression = key_expression & Key("timestamp").between(
            start_timestamp,
            end_timestamp,
        )
    elif start_timestamp:
        key_expression = key_expression & Key("timestamp").gte(start_timestamp)
    elif end_timestamp:
        key_expression = key_expression & Key("timestamp").lte(end_timestamp)

    result = table.query(
        KeyConditionExpression=key_expression,
        ScanIndexForward=ascending,
        Limit=limit,
    )

    return {
        "satellite_id": satellite_id,
        "count": len(result.get("Items", [])),
        "items": result.get("Items", []),
        "last_evaluated_key": result.get("LastEvaluatedKey"),
    }


def get_telemetry_at_timestamp(satellite_id, timestamp):
    result = table.get_item(
        Key={
            "satellite_id": satellite_id,
            "timestamp": timestamp,
        }
    )

    return result.get("Item")


def lambda_handler(event, context):
    logger.info("Incoming event: %s", json.dumps(event, default=str))

    try:
        method, path_parts, query = parse_event(event)

        if method == "OPTIONS":
            return response(204, {})

        if method not in ("GET", "POST"):
            return response(405, {"error": "Method not allowed"})

        if (
            method == "POST"
            and (
                len(path_parts) == 3
                or (len(path_parts) == 4 and path_parts[2] == "commands")
            )
            and path_parts[0] == "satellites"
            and path_parts[-1] == "led"
        ):
            return response(202, publish_led_command(path_parts[1], parse_json_body(event)))

        if method != "GET":
            return response(405, {"error": "Method not allowed"})

        if path_parts == ["health"]:
            return response(200, {
                "ok": True,
                "table": TABLE_NAME,
            })

        if path_parts == ["satellites"]:
            return response(200, list_satellites())

        if (
            len(path_parts) == 3
            and path_parts[0] == "satellites"
            and path_parts[2] == "latest"
        ):
            satellite_id = path_parts[1]
            item = get_latest_telemetry(satellite_id)

            if item is None:
                return response(404, {
                    "error": "Satellite telemetry not found",
                    "satellite_id": satellite_id,
                })

            return response(200, item)

        if (
            len(path_parts) == 3
            and path_parts[0] == "satellites"
            and path_parts[2] == "telemetry"
        ):
            return response(200, get_telemetry(path_parts[1], query))

        if (
            len(path_parts) == 4
            and path_parts[0] == "satellites"
            and path_parts[2] == "telemetry"
        ):
            satellite_id = path_parts[1]
            timestamp = path_parts[3]
            item = get_telemetry_at_timestamp(satellite_id, timestamp)

            if item is None:
                return response(404, {
                    "error": "Telemetry item not found",
                    "satellite_id": satellite_id,
                    "timestamp": timestamp,
                })

            return response(200, item)

        return response(404, {
            "error": "Route not found",
            "path": "/" + "/".join(path_parts),
        })

    except ValueError as error:
        logger.info("Bad request: %s", error)
        return response(400, {
            "error": "Bad request",
            "message": str(error),
        })

    except AwsIotPublishError as error:
        logger.exception("AWS IoT publish error")
        return response(502, {
            "error": "AWS IoT publish failed",
            "message": str(error),
        })

    except ClientError as error:
        code, message = client_error_details(error)
        logger.exception("AWS client error code=%s message=%s", code, message)
        return response(500, {
            "error": "AWS client error",
            "code": code,
            "message": message,
        })

    except Exception as error:
        logger.exception("Internal server error")
        return response(500, {
            "error": "Internal server error",
            "message": str(error),
        })