# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wt32-bridge)

# How many NAT sessions the WAN uplink can carry at once. lwIP sizes its NAPT
# table from IP_NAPT_MAX, which has no Kconfig symbol - it is a bare #ifndef in
# lwip/lwip_napt.h, so the only way to change it is a compile definition on the
# lwIP component. The entry is 32 bytes, making the 512 default 16 KB of heap
# claimed on the first esp_netif_napt_enable() and never given back.
#
# 256 (8 KB) rather than the default: six WiFi stations plus a handful of wired
# devices, each with a couple of NTRIP sessions and DNS, is tens of entries. The
# headroom above that is deliberate, because the egress allowlist filters
# *after* translation - ip4_forward() calls ip_napt_forward() before
# netif->output(), so a packet this device is about to drop has already taken a
# table entry - and ip_napt_gc() evicts the least recently used when the table
# fills. Running out is not a queue, it is a live NTRIP session dying.
#
# PRIVATE because IP_NAPT_MAX is only dereferenced inside ip4_napt.c; nothing
# outside the lwIP component sizes anything from it.
#
# IP_NAPT_TIMEOUT_MS_UDP: lwIP expires a UDP mapping after two seconds, which is
# far shorter than any real NAT and is fine only if UDP means DNS, where the
# reply arrives in milliseconds. RustDesk's ID registration and heartbeat are
# UDP, and the server reaches a client by poking the mapping that heartbeat
# established - so a two-second lifetime means the client is unreachable in the
# gaps between beats, which is most of the time.
#
# 60 s comfortably covers RustDesk's heartbeat interval and is at the
# conservative end of what a consumer NAT would use. The cost is table
# occupancy: entries live longer, against the IP_NAPT_MAX above.
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${lwip_lib} PRIVATE
    IP_NAPT_MAX=256
    IP_NAPT_TIMEOUT_MS_UDP=60000)

