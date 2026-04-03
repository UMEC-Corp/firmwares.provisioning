cmake_minimum_required(VERSION 3.16)
set(EXCLUDE_COMPONENTS
    modbus_gateway
    modbus_inspector
    modbus_rs485
    modbus_scanner
    modbus_server
    mqtt_client
    scenarios
    test_agent
    utils
    wifi_ble
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(umec-core)
