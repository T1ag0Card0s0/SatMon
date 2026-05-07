# SatMon

Satellite cluster monitoring system simulated with IoT nodes.

This project was developed in the scope of Master's degree in Eletrical and Computer Engineering at University of Coimbra.

## Setup

1. Clone esp-idf repository:

```bash
git clone https://github.com/espressif/esp-idf.git ~/.esp-idf
```
2. Setup esp-idf:

```bash
cd ~/.esp-idf/ && git submodule update --init --recursive && ./install.sh 
```

## Convenience scripts

Each project area has a local shell entry point:

```bash
./run.sh
./broker/run.sh
./firmware/run.sh
./frontend/run.sh
```

From the project root, `./run.sh` starts the full Docker stack: Mosquitto, the broker cloud bridge, and the frontend dashboard. The frontend serves at `http://localhost:5173` by default and loads telemetry from the REST API through a local `/api` proxy. Override the frontend port with `SATMON_FRONTEND_PORT` and the upstream API with `SATMON_API_TARGET` in `broker/.env`.

## Compile

1. Set esp-idf environment:

```bash
. ~/.esp-idf/export.sh
```

2. Set target and build:

```bash 
cd firmware && idf.py set-target esp32c6 && idf.py build
```

Since our firmware/ folder already contains the sdkconfig there is no need to run the idf.py set-target command.
If the set-target command is executed it will reset the working sdkconfig.

## Flash

1. To flash the esp32c6 board, after configuring and compiling the project, run the following command:

```bash
idf.py -p <Device usualy /dev/ttyUSB0> flash
```

2. To check for the logs run:

```bash
idf.py -p <Same device used to flash> monitor
```

## Run the hardware

To run the firmware, use `idf.py menuconfig` and set these values under "SatMon Configuration":

CONFIG_SATMON_WIFI_SSID
CONFIG_SATMON_WIFI_PASSWORD
CONFIG_SATMON_MQTT_BROKER_URI

## Run the MQTT broker bridge

The ESP32 nodes publish to a local MQTT broker, and the broker bridge receives every local MQTT topic with `#`. Messages are forwarded to the configured cloud target; in AWS IoT mode, node payloads are published to `satmon/satellites`. LED commands travel the other way: the dashboard posts to the cloud API, the cloud publishes to `satmon/commands/<satellite_id>/led`, and the bridge republishes that command to the local broker for the ESP32-C6 board.

The firmware also listens on `satmon/commands/all/led`, which is useful for testing the LED path when you are not sure which node profile is flashed. Built-in profiles fix the command ID: SAT-01 listens as `SAT-01`, SAT-02 listens as `SAT-02`, and `CONFIG_SATMON_SATELLITE_ID` only applies to the Custom profile.

To run the broker and dashboard together:

```bash
./run.sh
```

The dashboard will be available at `http://localhost:5173`.

To run only the broker stack:

```bash
cd broker
./run.sh
```

`./run.sh` creates the broker `.env` file on first run, starts Mosquitto, starts the Python bridge, and streams MQTT communication logs in the terminal. The default cloud target is `SATMON_CLOUD_TARGET=log` for local testing. To forward to AWS IoT, set `SATMON_CLOUD_TARGET=aws_iot` and replace `SATMON_AWS_IOT_ENDPOINT` in `broker/.env` with the current AWS IoT Core endpoint for the certificate bundle under `aws_certs/`.

For LED commands, deploy the HTTP Lambda with `AWS_IOT_DATA_ENDPOINT` or `SATMON_AWS_IOT_ENDPOINT` set to the AWS IoT data endpoint, and allow the Lambda role to call `iot:Publish` on `satmon/commands/*`.

## Run in qemu

1. Install espressif qemu:

```bash
python $IDF_PATH/tools/idf_tools.py install qemu-riscv32
```

After this installation you can check if it is correctly install by re-running the same command.

2. Add qemu to PATH list, write in .bashrc file the following line:

```bash
export PATH="/home/tr-cardoso/.espressif/tools/qemu-riscv32/esp_develop_<QEMU Version>/qemu/bin:$PATH"
```

3. To launch in qemu, note that qemu only supports esp32, esp32s3 and esp32c3, so after configuring target for esp32c3 and building, run the following command:

```bash 
idf.py qemu
```


