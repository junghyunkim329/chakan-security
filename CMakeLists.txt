cmake_minimum_required(VERSION 3.25)
project(vehicle_sil LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# project.md §15.1: warnings are errors
add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wshadow
	$<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
	$<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ---- CAN signal tables generated from the single YAML definition (project.md §5.3) ----
set(SIL_SIGNALS_YAML ${CMAKE_SOURCE_DIR}/config/signals/can.yaml)
set(SIL_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_command(
	OUTPUT ${SIL_GEN_DIR}/sil_can_signals_gen.h ${SIL_GEN_DIR}/sil_can_signals_gen.c
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/codegen/gen_can_signals.py ${SIL_SIGNALS_YAML} ${SIL_GEN_DIR}
	DEPENDS ${SIL_SIGNALS_YAML}
		${CMAKE_SOURCE_DIR}/tools/codegen/gen_can_signals.py
		${CMAKE_SOURCE_DIR}/tools/sil_can/definitions.py
	COMMENT "Generating CAN signal tables from config/signals/can.yaml"
	VERBATIM)

add_library(sil_can STATIC
	${SIL_GEN_DIR}/sil_can_signals_gen.c
	ECU/src/common/crc8.c
	ECU/src/common/pdu_codec.c)
target_include_directories(sil_can PUBLIC ECU/src/common/include ${SIL_GEN_DIR})
target_link_libraries(sil_can PUBLIC m)

add_library(sil_scenario STATIC ECU/src/common/scenario.c)
target_include_directories(sil_scenario PUBLIC ECU/src/common/include)
target_link_libraries(sil_scenario PUBLIC m)

# ---- diagnostics: tables generated from config/diagnostics/uds.yaml (project.md §9) ----
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
add_custom_command(
	OUTPUT ${SIL_GEN_DIR}/sil_diag_gen.h ${SIL_GEN_DIR}/sil_diag_gen.c
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/codegen/gen_diag.py ${CMAKE_SOURCE_DIR}/config/diagnostics/uds.yaml ${SIL_GEN_DIR}
	DEPENDS ${CMAKE_SOURCE_DIR}/config/diagnostics/uds.yaml ${CMAKE_SOURCE_DIR}/tools/codegen/gen_diag.py
		${CMAKE_SOURCE_DIR}/tools/sil_can/diagnostics.py
	COMMENT "Generating diagnostic tables from config/diagnostics/uds.yaml"
	VERBATIM)
add_library(sil_uds STATIC ${SIL_GEN_DIR}/sil_diag_gen.c ECU/src/common/uds_server.c)
target_include_directories(sil_uds PUBLIC ECU/src/common/include ${SIL_GEN_DIR})
target_link_libraries(sil_uds PUBLIC OpenSSL::Crypto)

# ---- LIN body domain: tables generated from config/lin/network.yaml (project.md §6) ----
add_custom_command(
	OUTPUT ${SIL_GEN_DIR}/sil_lin_gen.h ${SIL_GEN_DIR}/sil_lin_gen.c
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/codegen/gen_lin.py ${CMAKE_SOURCE_DIR}/config/lin/network.yaml ${SIL_GEN_DIR}
	DEPENDS ${CMAKE_SOURCE_DIR}/config/lin/network.yaml ${CMAKE_SOURCE_DIR}/tools/codegen/gen_lin.py
		${CMAKE_SOURCE_DIR}/tools/sil_can/lin.py
	COMMENT "Generating LIN tables from config/lin/network.yaml"
	VERBATIM)
add_library(sil_lin STATIC ${SIL_GEN_DIR}/sil_lin_gen.c ECU/src/common/lin.c ECU/src/lin/lin_slave.c)
target_include_directories(sil_lin PUBLIC ECU/src/common/include ECU/src/lin ${SIL_GEN_DIR})
add_executable(lin_slave ECU/src/lin/slave_main.c)
target_link_libraries(lin_slave PRIVATE sil_lin)

add_library(sil_heartbeat STATIC ECU/src/common/heartbeat.c)
target_include_directories(sil_heartbeat PUBLIC ECU/src/common/include)

add_library(sil_ecu_runtime STATIC ECU/src/common/ecu_runtime.c)
target_link_libraries(sil_ecu_runtime PUBLIC sil_can sil_scenario sil_heartbeat sil_uds)

foreach(ecu engine brake steering)
	add_executable(ecu_${ecu} ECU/src/ecu/${ecu}/main.c)
	target_link_libraries(ecu_${ecu} PRIVATE sil_ecu_runtime)
endforeach()

# ---- SOME/IP service definitions generated from config/services/*.yaml (project.md §7.4) ----
file(GLOB SIL_SERVICE_YAMLS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/config/services/*.yaml)
add_custom_command(
	OUTPUT ${SIL_GEN_DIR}/sil_services_gen.hpp
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/codegen/gen_services.py ${SIL_GEN_DIR} ${SIL_SERVICE_YAMLS}
	DEPENDS ${SIL_SERVICE_YAMLS} ${CMAKE_SOURCE_DIR}/tools/codegen/gen_services.py
	COMMENT "Generating SOME/IP service serializers from config/services"
	VERBATIM)
add_custom_target(sil_services_gen DEPENDS ${SIL_GEN_DIR}/sil_services_gen.hpp)
add_library(sil_services INTERFACE)
target_include_directories(sil_services INTERFACE ${SIL_GEN_DIR})
add_dependencies(sil_services sil_services_gen)

# ---- Vehicle Gateway ----
add_library(sil_gateway STATIC
	ECU/src/gateway/can_input_validator.cpp
	ECU/src/gateway/vehicle_state_cache.cpp
	ECU/src/gateway/lin_master.cpp
	ECU/src/gateway/body_control.cpp)
target_include_directories(sil_gateway PUBLIC ECU/src/gateway)
target_link_libraries(sil_gateway PUBLIC sil_can sil_services sil_lin)

# ---- Execution Manager (project.md §8) ----
add_library(sil_em STATIC ECU/src/adaptive/execution-manager/supervisor.cpp)
target_include_directories(sil_em PUBLIC ECU/src/adaptive/execution-manager)

# manifest parsing uses Boost.PropertyTree (header-only) from the pinned local Boost (scripts/setup/build-vsomeip.sh)
set(SIL_BOOST_INCLUDE ${CMAKE_SOURCE_DIR}/third_party/boost-root/usr/include)
if(EXISTS ${SIL_BOOST_INCLUDE}/boost/property_tree/json_parser.hpp)
	add_library(sil_em_manifest STATIC ECU/src/adaptive/execution-manager/manifest.cpp)
	target_include_directories(sil_em_manifest SYSTEM PUBLIC ${SIL_BOOST_INCLUDE})
	target_link_libraries(sil_em_manifest PUBLIC sil_em)
	add_executable(execution_manager ECU/src/adaptive/execution-manager/em_main.cpp)
	target_link_libraries(execution_manager PRIVATE sil_em_manifest)
	add_executable(test_manifest ECU/tests/unit/test_manifest.cpp)
	target_include_directories(test_manifest PRIVATE ECU/tests/unit)
	target_link_libraries(test_manifest PRIVATE sil_em_manifest)
	add_test(NAME test_manifest COMMAND test_manifest ${CMAKE_SOURCE_DIR}/config/manifests/execution.json)
else()
	message(STATUS "Boost headers not found in third_party: execution_manager is skipped")
endif()

# ---- DoIP entity in the Gateway (project.md §9.1) ----
add_library(sil_doip STATIC ECU/src/gateway/doip.cpp)
target_include_directories(sil_doip PUBLIC ECU/src/gateway ${SIL_GEN_DIR})
target_link_libraries(sil_doip PUBLIC sil_uds)
add_executable(doip_gateway ECU/src/gateway/doip_main.cpp)
target_link_libraries(doip_gateway PRIVATE sil_doip)

# ---- SOME/IP processes (need vSomeIP built into third_party/vsomeip, see scripts/setup/build-vsomeip.sh) ----
find_package(vsomeip3 CONFIG QUIET PATHS ${CMAKE_SOURCE_DIR}/third_party/vsomeip NO_DEFAULT_PATH)
if(vsomeip3_FOUND)
	find_package(Threads REQUIRED)
	add_executable(vehicle_gateway ECU/src/gateway/gateway_main.cpp)
	target_link_libraries(vehicle_gateway PRIVATE sil_gateway sil_heartbeat sil_uds vsomeip3 Threads::Threads)
	add_executable(body_control_client ECU/src/adaptive/body-control/client_main.cpp)
	target_link_libraries(body_control_client PRIVATE sil_services vsomeip3 Threads::Threads)
	add_executable(vehicle_state_consumer ECU/src/adaptive/vehicle-state/consumer_main.cpp)
	target_link_libraries(vehicle_state_consumer PRIVATE sil_services sil_heartbeat vsomeip3 Threads::Threads)
else()
	message(STATUS "vSomeIP not found in third_party/vsomeip: vehicle_gateway and vehicle_state_consumer are skipped")
endif()

# ---- tests ----
enable_testing()

add_executable(test_supervisor ECU/tests/unit/test_supervisor.cpp)
target_include_directories(test_supervisor PRIVATE ECU/tests/unit)
target_link_libraries(test_supervisor PRIVATE sil_em)
add_test(NAME test_supervisor COMMAND test_supervisor)

add_executable(test_doip ECU/tests/unit/test_doip.cpp)
target_include_directories(test_doip PRIVATE ECU/tests/unit)
target_link_libraries(test_doip PRIVATE sil_doip)
add_test(NAME test_doip COMMAND test_doip)

foreach(t test_can_input_validator test_vehicle_state_cache test_vehicle_state_payload test_lin_master)
	add_executable(${t} ECU/tests/unit/${t}.cpp)
	target_include_directories(${t} PRIVATE ECU/tests/unit)
	target_link_libraries(${t} PRIVATE sil_gateway)
	add_test(NAME ${t} COMMAND ${t})
endforeach()

add_executable(test_lin ECU/tests/unit/test_lin.c)
target_include_directories(test_lin PRIVATE ECU/tests/unit)
target_link_libraries(test_lin PRIVATE sil_lin)
add_test(NAME test_lin COMMAND test_lin)

add_executable(test_uds_server ECU/tests/unit/test_uds_server.c)
target_include_directories(test_uds_server PRIVATE ECU/tests/unit)
target_link_libraries(test_uds_server PRIVATE sil_uds)
add_test(NAME test_uds_server COMMAND test_uds_server)

foreach(t test_crc8 test_codec test_scenario)
	add_executable(${t} ECU/tests/unit/${t}.c)
	target_include_directories(${t} PRIVATE ECU/tests/unit)
	target_link_libraries(${t} PRIVATE sil_can sil_scenario)
	add_test(NAME ${t} COMMAND ${t})
endforeach()

add_executable(codec_vectors ECU/tests/unit/codec_vectors.c)
target_link_libraries(codec_vectors PRIVATE sil_can)

# C codec and Python reference codec must produce identical bytes from the same YAML
add_test(NAME python_codec
	COMMAND ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider ${CMAKE_SOURCE_DIR}/ECU/tests/unit/python)
set_tests_properties(python_codec PROPERTIES
	ENVIRONMENT "SIL_CODEC_VECTORS=$<TARGET_FILE:codec_vectors>")
