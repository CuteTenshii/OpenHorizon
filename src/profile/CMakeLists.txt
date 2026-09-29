add_library(fh1profile STATIC
    Profile.cpp
    Profile.h
    ProfileSchema.cpp
    ProfileSchema.h
    ProfileStore.cpp
    ProfileStore.h
)
target_compile_options(fh1profile PRIVATE ${FH1N_WARNINGS})
# fh1base provides the VFS the schema is read through, and logging.
target_link_libraries(fh1profile PUBLIC fh1base pugixml::pugixml)
