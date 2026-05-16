# AT MQTT / Wi-Fi UART1

Use this skill when the user wants the device to control an ESP-AT style modem or host channel over UART1.

Tools:

- `at_uart1_configure`: configure UART1 pins and baud rate.
- `at_send_command`: send an arbitrary AT command and return the UART response.
- `at_wifi_set_mode`: send `AT+CWMODE=<mode>`. Use `1` for STA, `2` for AP, `3` for STA+AP.
- `at_wifi_join_ap`: send `AT+CWJAP="<ssid>","<password>"`.
- `at_wifi_provision_sta`: set STA mode, then join the AP.
- `at_mqtt_usercfg`: send `AT+MQTTUSERCFG`.
- `at_mqtt_connect`: send `AT+MQTTCONN`.
- `at_mqtt_subscribe`: send `AT+MQTTSUB`.
- `at_mqtt_publish_raw`: send `AT+MQTTPUBRAW` and the payload.
- `at_mqtt_huawei_setup`: helper for Huawei IoTDA MQTT. All credentials and topics must be provided by the user at runtime.

Never invent MQTT credentials, Wi-Fi credentials, hostnames, or topics. Use placeholders such as `your-device-client-id`, `your-device-password`, and `$oc/devices/your-device-id/sys/properties/report` in examples.
