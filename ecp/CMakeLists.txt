# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
cmake_minimum_required(VERSION 3.16)
project(chiaki-ecp C)

add_executable(chiaki-ecp
    chiaki_ecp.c
    ecp.c
)
set_target_properties(chiaki-ecp PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
if(WIN32)
    target_link_libraries(chiaki-ecp PRIVATE ws2_32 advapi32)
else()
    target_link_libraries(chiaki-ecp PRIVATE m pthread)
endif()
