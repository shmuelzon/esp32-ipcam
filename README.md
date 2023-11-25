# ESP32-IPCAM

This project is a simple ESP32-based IP camera. It captures JPEG images from a
camera supported by the [ESP32 Camera Driver](https://github.com/espressif/esp32-camera)
and sends them, over RTP to a predefined destination. It's based on
[ESP-IDF](https://github.com/espressif/esp-idf) release v5.1.1. Note that using
any other ESP-IDF version might not be stable or even compile.

IPCAM also supports PDM microphones and encodes the audio using the Opus audio
codec and transmits that as well. It's also possible to connect a motion sensor
and expose its status over MQTT.

It includes a very simple management web UI and a couple of additional URL:
* http://<IP address>/still - Returns a single JPEG image
* http://<IP address>/stream - Returns an SDP for reading the video stream. This
  URL can be used as an input for a video player, e.g., FFmpeg or VLC, to view
  the captured video stream

The IPCAM devices can also connect to an MQTT bus and publish the following
topics to help book-keeping:
* `IPCAM-XXXX/MotionDetected` - With a payload of `true`/`false` depicting if
  motion was detected
* `IPCAM-XXX/Version` - The IPCAM application version currently running
* `IPCAM-XXX/ConfigVersion` - The IPCAM configuration version currently loaded
  (MD5 hash of configuration file)
* `IPCAM-XXX/Uptime` - The uptime of the ESP32, in seconds, published every
  minute
* `IPCAM-XXX/Status` - `Online` when running, `Offline` when powered off
  (the latter is an LWT message)

## Compiling

1. Install `ESP-IDF`

You will first need to install the
[Espressif IoT Development Framework](https://github.com/espressif/esp-idf).
The [Installation Instructions](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/linux-macos-setup.html)
have all of the details. Make sure to follow ALL the steps, up to and including step 4 where you set up the tools and
the `get_idf` alias.

2. Download the repository and its dependencies:

```bash
git clone --recursive https://github.com/shmuelzon/esp32-ipcam
```

3. Modify the config.json and flash

Modify the [configuration file](#configuration) to fit your environment, build
and flash (make sure to modify the serial device your ESP32 is connected to):

```bash
idf.py build flash
```

## Remote Logging

If configured, the application can send the logs remotely via UDP to another
host to allow receiving logs from remote devices without a serial connection.
To receive these logs on your host, execute `idf.py remote-monitor`.

## Configuration

The configuration file provided in located at
[data/config.json](data/config.json) in the repository. It contains all of the
different configuration options.

The `network` section should contain either a `wifi` section or an `eth`
section.  If case there are both, the `eth` section has preference over the
`wifi` section.

Optionally, the network section can contain a `hostname` which, if set,
is used in MQTT subscriptions as well. In such case, replace `IPCAM-XXXX` in
this documentation with the host name you have set.

The `wifi` section below includes the following entries:
```json
{
  "network": {
    "hostname": "MY_HOSTNAME",
    "wifi": {
      "ssid": "MY_SSID",
      "password": "MY_PASSWORD",
      "eap": {
        "method": null,
        "identity": null,
        "client_cert": null,
        "client_key": null,
        "server_cert": null,
        "username": null,
        "password": null
      }
    }
  }
}
```
* `ssid` - The WiFi SSID the ESP32 should connect to
* `password` - The security password for the above network
* `eap` - WPA-Enterprise configuration (for enterprise networks only)
  * `method` - `TLS`, `PEAP` or `TTLS`
  * `identity` - The EAP identity
  * `ca_cert`, `client_cert`, `client_key` - Full path names, including a
    leading slash (/), of the certificate/key file (in PEM format) stored under
    the data folder
  * `username`, `password` - EAP login credentials

The `eth` section below includes the following entries:
```json
{
  "network": {
    "eth": {
      "phy": "MY_ETH_PHY",
      "phy_power_pin": -1
    }
  }
}
```
* `phy` - The PHY chip connected to ESP32 RMII, one of:
  * `IP101`
  * `RTL8201`
  * `LAN8720`
  * `DP83848`
* `phy_power_pin` - Some ESP32 Ethernet modules such as the Olimex ESP32-POE require a GPIO pin to be set high in order to enable the PHY. Omitting this configuration or setting it to -1 will disable this.

_Note: Defining the `eth` section will disable Wi-Fi_

The `rtp` section below includes the following entries:
```json
{
  "rtp": {
    "host": "225.5.5.5",
    "video_port": 5000,
    "audio_port": 5002,
    "ttl": 1
  }
}
```
* `host` - The IP address for publishing the audio/video stream
* `video_port` - The UDP port for the video RTP packets (even port number)
* `audio_port` - The UDP port for the audio RTP packets (even port number)
* `ttl` - The time-to-live entry of the UDP packet

The `camera` section below includes the following entries:
```json
{
  "camera": {
    "pins": {
      "pwdn": -1,
      "reset": -1,
      "xclk": 10,
      "siod": 40,
      "sioc": 39,
      "d7": 48,
      "d6": 11,
      "d5": 12,
      "d4": 14,
      "d3": 16,
      "d2": 18,
      "d1": 17,
      "d0": 15,
      "vsync": 38,
      "href": 47,
      "pclk": 13
    },
    "resolution": "800x600",
    "fps": 10,
    "vertical_flip": true,
    "horizontal_mirror": true,
    "quality": 12
}
```
* `pins` - The camera pin configuration for the ESP module. See below the
  configuration for a few common modules
* `resolution` - The image size to capture. One of: `96x96`, `160x120`,
  `176x144`, `240x176`, `240x240`, `320x240`, `400x296`, `480x320`, `640x480`,
  `800x600`, `1024x768`, `1280x720`, `1280x1024`, `1600x1200`, `1920x1080`,
  `720x1280`, `864x1536`, `2048x1536`, `2560x1440`, `2560x1600`, `1080x1920`,
  `2560x1920`
* `fps` - Frames per second to capture
* `vertical_flip` - `true`/`false` whether the image should be flipped
* `horizontal_mirror` - `true`/`false` whether the image should mirrored
* `quality` - The JPEG image compression quality (0-63) where a lower value is
  higher quality

The `microphone` section below includes the following entries:
```json
{
  "microphone": {
    "clk": -1,
    "din": -1,
    "sample_rate": 48000
  },
}
```
* `clk` - The clock pin of the PDM microphone
* `din` - The data pin of the PDM microphone
* `sample_rate` - The capture sample rate

The `motion` section below includes the following entries:
```json
{
  "_motion_sensor": {
    "pin": 1
  }
}
```
* `pin` - The GPIO the motion sensor is connected to

The `mqtt` section below includes the following entries:
```json
{
  "mqtt": {
    "server": {
      "host": "192.168.1.1",
      "port": 1883,
      "ssl": false,
      "client_cert": null,
      "client_key": null,
      "server_cert": null,
      "username": null,
      "password": null,
      "client_id": null
    },
    "publish": {
      "qos": 0,
      "retain": true
    }
  }
}
```
* `server` - MQTT connection parameters
  * `host` - Host name or IP address of the MQTT broker
  * `port` - TCP port of the MQTT broker. If not specificed will default to
    1883 or 8883, depending on SSL configuration
  * `client_cert`, `client_key`, `server_cert` - Full path names, including a
    leading slash (/), of the certificate/key file (in PEM format) stored under
    the data folder. For example, if a certificate file is placed at
    `data/certs/my_cert.pem`, the value stored in the configuration should be
    `/certs/my_cert.pem`
  * `username`, `password` - MQTT login credentials
  * `client_id` - The MQTT client ID
* `publish` - Configuration for publishing topics

The optional `log` section below includes the following entries:
```json
{
  "log": {
    "host": "224.0.0.200",
    "port": 5000
  }
}
```
* `host` - The hostname or IP address to send the logs to. In case of an IP
  address, this may be a unicast, broadcast or multicast address
* `port` - The destination UDP port

## OTA

It is possible to upgrade both firmware and configuration file over-the-air once
an initial version was flashed via serial interface. To do so, execute:
`idf.py upload` or `idf.py upload-config` accordingly.
The above will upgrade all IPCAM devices connected to the MQTT broker defined in
the configuration file. It is also possible to upgrade a specific device by
adding the `OTA_TARGET` variable to the above command set to the host name of
the requested device, e.g.:
```bash
OTA_TARGET=IPCAM-1234 idf.py upload
```

Note: In order to avoid unneeded upgrades, there is a mechanism in place to
compare the new version with the one that resides on the flash. For the firmware
image it's based on the git tag and for the configuration file it's an MD5 hash
of its contents. In order to force an upgrade regardless of the currently
installed version, run `idf.py force-upload` or `idf.py force-upload-config`
respectively.

## Module Compatibility
The following sections include the camera configuration for common modules

### ESP-CAM
```json
{
  "camera": {
    "pins": {
      "pwdn": 32,
      "reset": -1,
      "xclk": 0,
      "siod": 26,
      "sioc": 27,
      "d7": 35,
      "d6": 34,
      "d5": 39,
      "d4": 36,
      "d3": 21,
      "d2": 19,
      "d1": 18,
      "d0": 5,
      "vsync": 25,
      "href": 23,
      "pclk": 22
    }
  }
}
```

### M5Stack
```json
{
  "camera": {
    "pins": {
      "pwdn": 0,
      "reset": 15,
      "xclk": 27,
      "siod": 22,
      "sioc": 23,
      "d7": 19,
      "d6": 36,
      "d5": 18,
      "d4": 39,
      "d3": 5,
      "d2": 34,
      "d1": 35,
      "d0": 32,
      "vsync": 25,
      "href": 26,
      "pclk": 21
    }
  }
}
```

### ESP-WROVER-KIT
```json
{
  "camera": {
    "pins": {
      "pwdn": -1,
      "reset": -1,
      "xclk": 21,
      "siod": 26,
      "sioc": 27,
      "d7": 35,
      "d6": 34,
      "d5": 39,
      "d4": 36,
      "d3": 19,
      "d2": 18,
      "d1": 5,
      "d0": 4,
      "vsync": 25,
      "href": 23,
      "pclk": 22
    }
  }
}
```

### XIAO ESP32S3 Sense
```json
{
  "camera": {
    "pins": {
      "pwdn": -1,
      "reset": -1,
      "xclk": 10,
      "siod": 40,
      "sioc": 39,
      "d7": 48,
      "d6": 11,
      "d5": 12,
      "d4": 14,
      "d3": 16,
      "d2": 18,
      "d1": 17,
      "d0": 15,
      "vsync": 38,
      "href": 47,
      "pclk": 13
    }
  },
  "microphone": {
    "clk": 42,
    "din": 41,
    "sample_rate": 48000
  }
}
```
