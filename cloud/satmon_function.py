import json
import os
from decimal import Decimal

import boto3


AWS_REGION = os.environ.get("AWS_REGION", "eu-west-1")
TABLE_NAME = os.environ.get("TABLE_NAME", "SatMonDB")

dynamodb = boto3.resource("dynamodb", region_name=AWS_REGION)
table = dynamodb.Table(TABLE_NAME)


def to_dynamodb_value(value):
    if isinstance(value, float):
        return Decimal(str(value))

    if isinstance(value, dict):
        return {
            key: to_dynamodb_value(child_value)
            for key, child_value in value.items()
            if child_value is not None
        }

    if isinstance(value, list):
        return [to_dynamodb_value(item) for item in value]

    return value


def lambda_handler(event, context):
    ddb_event = json.loads(json.dumps(event), parse_float=Decimal)

    print("Lambda region:", AWS_REGION)
    print("DynamoDB table:", TABLE_NAME)
    print("Received event:", json.dumps(event))

    satellite_id = ddb_event.get("satellite_id")
    timestamp = ddb_event.get("timestamp")
    sensors = ddb_event.get("sensors")

    if not satellite_id:
        raise ValueError("Missing satellite_id")

    if not timestamp:
        raise ValueError("Missing timestamp")

    if not isinstance(sensors, dict):
        raise ValueError("Missing or invalid sensors object")

    item = {
        "id": str(satellite_id),
        "ts": str(timestamp),
        "satellite_id": str(satellite_id),
        "timestamp": str(timestamp),
        "sensors": to_dynamodb_value(sensors),
    }

    print("Insert SatMon event in DynamoDB:", json.dumps(item, default=str))

    table.put_item(Item=item)

    print("Success")

    return {
        "statusCode": 200,
        "body": json.dumps({
            "message": "Success",
            "table": TABLE_NAME,
            "satellite_id": str(satellite_id),
            "timestamp": str(timestamp),
        }),
    }