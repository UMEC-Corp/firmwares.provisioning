if(NOT DEFINED UMEC_PROFILE)
    set(UMEC_PROFILE "core")
endif()

file(GLOB_RECURSE APP_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../src/${UMEC_PROFILE}/*.cpp
)

idf_component_register(
    SRCS         ${APP_SOURCES}
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/../src/${UMEC_PROFILE}
    REQUIRES     core
)
