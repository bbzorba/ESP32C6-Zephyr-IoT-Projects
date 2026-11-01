# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Wi-Fi credentials live in wifi.conf (git-ignored), see wifi.conf.example
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/wifi.conf)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/wifi.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(web_app)

target_sources(app PRIVATE
  src/main.c
  src/wifi.c
  src/lan_scan.c
  src/sensors.c
  src/bme688.c
  src/nodes.c
  src/mqtt_pub.c
  src/ota.c
  src/bl_update.c
  src/web_api.c
)

# The ARP cache API (net_arp_foreach) is internal to the Ethernet L2
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/subsys/net/l2/ethernet)

# Iterable section holding the HTTP resources of hub_http_service
zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(NAME http_resource_desc_hub_http_service
                      KVMA RAM_REGION GROUP RODATA_REGION)

# Public key that bl_update.c checks staged bootloaders against, taken from
# the key that signs the firmware (the private key never enters the image)
set(SIGNING_KEY ${CMAKE_CURRENT_LIST_DIR}/keys/hub-signing-key.pem)
set(PUBKEY_DER ${ZEPHYR_BINARY_DIR}/hub-signing-key.pub.der)
execute_process(
  COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_MCUBOOT_MODULE_DIR}/scripts/imgtool.py
          getpub -k ${SIGNING_KEY} -e raw -o ${PUBKEY_DER}
  COMMAND_ERROR_IS_FATAL ANY
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SIGNING_KEY})
generate_inc_file_for_target(app ${PUBKEY_DER}
  ${ZEPHYR_BINARY_DIR}/include/generated/bl_pubkey.inc)
# MCUboot's image header and TLV definitions (bootutil/image.h)
target_link_libraries(app PRIVATE MCUBOOT_BOOTUTIL)

# Web page is gzipped at build time and served from flash
generate_inc_file_for_target(
  app
  src/static_web_resources/index.html
  ${ZEPHYR_BINARY_DIR}/include/generated/index.html.gz.inc
  --gzip
)
