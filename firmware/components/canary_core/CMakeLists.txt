file(READ "${CMAKE_CURRENT_LIST_DIR}/../../version.txt" CANARY_VERSION)
string(STRIP "${CANARY_VERSION}" CANARY_VERSION)

idf_component_register(
    SRCS "src/util.c" "src/isotp.c" "src/diag.c" "src/expr.c" "src/profile.c" "src/config.c" "src/monitors.c"
         "src/trips.c" "src/vehicle.c" "src/sync.c" "src/api.c"
    INCLUDE_DIRS "include"
    REQUIRES json)
target_compile_definitions(${COMPONENT_LIB} PUBLIC CN_FW_VERSION="${CANARY_VERSION}")
