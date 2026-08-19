cmake_minimum_required(VERSION 3.16)
project(wtsn-configurator VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

include(CTest)
if(BUILD_TESTING)
  enable_testing()
endif()

option(BUILD_GUI "Build LVGL GUI front-end" ON)
option(BUILD_PLUGINS "Build sample protocol plugins" ON)
option(WTSN_ENABLE_PUBSUB "Use real OPC UA PubSub (open62541 must be built with UA_ENABLE_PUBSUB)" OFF)
if(WTSN_ENABLE_PUBSUB)
  add_compile_definitions(UA_ENABLE_PUBSUB=1)
endif()

find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)
pkg_check_modules(OPEN62541 REQUIRED open62541)

link_directories(${MOSQUITTO_LIBRARY_DIRS} ${OPEN62541_LIBRARY_DIRS})

find_package(Threads REQUIRED)

set(WTSN_SOURCES
  src/app/app.c
  src/common/log.c
  src/common/str_util.c
  src/mvc/model.c
  src/mvc/controller.c
  src/mvc/event_bus.c
  src/mvc/view.c
  src/db/db.c
  src/db/db_devices.c
  src/db/db_qos.c
  src/db/db_vlan.c
  src/db/db_tas.c
  src/db/db_timesync.c
  src/db/db_sensors.c
  src/device/device.c
  src/device/device_manager.c
  src/discovery/discovery.c
  src/qos/qos.c
  src/qos/qos_manager.c
  src/vlan/vlan.c
  src/vlan/vlan_manager.c
  src/timesync/timesync.c
  src/timesync/timesync_manager.c
  src/tas/tas.c
  src/tas/tas_manager.c
  src/tas/gcl.c
  src/sensors/sensor.c
  src/sensors/sensor_manager.c
  src/mqtt/mqtt_client.c
  src/opcua/opcua_server.c
  src/gateway/gateway.c
  src/gateway/gateway_pubsub.c
  src/pubsub/pubsub.c
  src/pubsub/pubsub_loopback.c
  src/pubsub/pubsub_opcua.c
  src/trace/trace.c
  src/plugin/plugin_manager.c
)

add_library(wtsn-core STATIC ${WTSN_SOURCES})
target_include_directories(wtsn-core PUBLIC src)
target_link_libraries(wtsn-core PRIVATE SQLite::SQLite3)
target_link_libraries(wtsn-core PUBLIC ${MOSQUITTO_LIBRARIES} ${OPEN62541_LIBRARIES})
target_include_directories(wtsn-core PUBLIC ${MOSQUITTO_INCLUDE_DIRS} ${OPEN62541_INCLUDE_DIRS})

if(BUILD_PLUGINS)
  add_library(plugin-mqtt-discovery SHARED src/plugin/plugin_mqtt_discovery.c)
  target_link_libraries(plugin-mqtt-discovery PRIVATE ${MOSQUITTO_LIBRARIES} wtsn-core)
  target_include_directories(plugin-mqtt-discovery PRIVATE ${MOSQUITTO_INCLUDE_DIRS})

  add_library(plugin-opcua-discovery SHARED src/plugin/plugin_opcua_discovery.c)
  target_link_libraries(plugin-opcua-discovery PRIVATE ${OPEN62541_LIBRARIES} wtsn-core)
  target_include_directories(plugin-opcua-discovery PRIVATE ${OPEN62541_INCLUDE_DIRS})
endif()

add_executable(wtsn-cli src/app/main_cli.c)
target_link_libraries(wtsn-cli PRIVATE wtsn-core SQLite::SQLite3 ${MOSQUITTO_LIBRARIES} ${OPEN62541_LIBRARIES} Threads::Threads)
target_include_directories(wtsn-cli PRIVATE ${MOSQUITTO_INCLUDE_DIRS} ${OPEN62541_INCLUDE_DIRS})



add_executable(wtsn-tests src/app/test_main.c)
target_link_libraries(wtsn-tests PRIVATE wtsn-core SQLite::SQLite3
  ${MOSQUITTO_LIBRARIES} ${OPEN62541_LIBRARIES} Threads::Threads)
target_include_directories(wtsn-tests PRIVATE
  ${MOSQUITTO_INCLUDE_DIRS} ${OPEN62541_INCLUDE_DIRS})
if(BUILD_TESTING)
  add_test(NAME wtsn-tests COMMAND wtsn-tests)
endif()

# ---- TSN Node Simulator (independent feature; does not touch core) ----
set(SIM_SOURCES
  src/simulator/common/sim_log.c
  src/simulator/common/sim_str.c
  src/simulator/core/sim_device.c
  src/simulator/core/sim_simulator.c
  src/simulator/profiles/sim_profile.c
  src/simulator/services/sim_services.c
  src/simulator/protocol/sim_protocol.c
  src/simulator/app/sim_main.c
)
add_executable(tsn-node-simulator ${SIM_SOURCES})
target_include_directories(tsn-node-simulator PRIVATE src)
target_link_libraries(tsn-node-simulator PRIVATE Threads::Threads)

# install simulator profiles into the build tree
file(COPY profiles DESTINATION ${CMAKE_BINARY_DIR})

# ---- TSN node firmware agent (runs on physical devices, executes configurator
#      commands via MQTT / OPC UA; Linux adapter uses iproute2/tc, embedded
#      adapters are compile-safe stubs for ESP-IDF/Zephyr/vendor SDK) ----
set(AGENT_SOURCES
  src/agent/agent.c
  src/agent/agent_providers.c
  src/agent/platform/agent_platform.c
  src/agent/agent_main.c
)
add_executable(tsn-node-agent ${AGENT_SOURCES})
target_include_directories(tsn-node-agent PRIVATE ${MOSQUITTO_INCLUDE_DIRS} src)
target_link_libraries(tsn-node-agent PRIVATE wtsn-core SQLite::SQLite3
  ${MOSQUITTO_LIBRARIES} ${OPEN62541_LIBRARIES} Threads::Threads)

