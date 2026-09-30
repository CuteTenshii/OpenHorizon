add_library(openhorizon_profile STATIC
    Profile.cpp
    Profile.h
    ProfileSchema.cpp
    ProfileSchema.h
    ProfileStore.cpp
    ProfileStore.h
)
target_compile_options(openhorizon_profile PRIVATE ${OPENHORIZON_WARNINGS})
# openhorizon_base provides the VFS the schema is read through, and logging.
target_link_libraries(openhorizon_profile PUBLIC openhorizon_base pugixml::pugixml)
