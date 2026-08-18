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

find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)
pkg_check_modules(OPEN62541 REQUIRED open62541)
pkg_check_modules(LVGL QUIET lvgl)

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

if(BUILD_GUI AND LVGL_FOUND)
  set(WTSN_UI_SOURCES
    src/ui/ui.c
    src/ui/widgets/theme.c
    src/ui/pages/dashboard_page.c
    src/ui/pages/default_pages.c
  )
  add_executable(wtsn-configurator src/app/main_gui.c ${WTSN_UI_SOURCES})
  target_link_libraries(wtsn-configurator PRIVATE wtsn-core SQLite::SQLite3
    ${MOSQUITTO_LIBRARIES} ${OPEN62541_LIBRARIES} Threads::Threads)
  target_include_directories(wtsn-configurator PRIVATE
    ${MOSQUITTO_INCLUDE_DIRS} ${OPEN62541_INCLUDE_DIRS} ${LVGL_INCLUDE_DIRS})
  target_compile_definitions(wtsn-configurator PRIVATE ${LVGL_DEFINITIONS})
else()
  message(WARNING "lvgl not found via pkg-config; building CLI only. "
                  "Install LVGL or set BUILD_GUI=OFF.")
endif()

add_executable(wtsn-tests src/app/test_main.c)
target_link_libraries(wtsn-tests PRIVATE wtsn-core SQLite::SQLite3
  ${MOSQUITTO_LIBRARIES} ${OPEN62541_LIBRARIES} Threads::Threads)
target_include_directories(wtsn-tests PRIVATE
  ${MOSQUITTO_INCLUDE_DIRS} ${OPEN62541_INCLUDE_DIRS})
if(BUILD_TESTING)
  add_test(NAME wtsn-tests COMMAND wtsn-tests)
endif()
