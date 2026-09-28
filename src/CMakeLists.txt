add_library(fh1base STATIC
    base/Bytes.cpp
    base/Bytes.h
    base/Error.h
    base/Log.cpp
    base/Log.h
    vfs/Vfs.cpp
    vfs/Vfs.h
)
target_include_directories(fh1base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(fh1base PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1base PUBLIC lzx::lzx pugixml::pugixml Threads::Threads)

add_executable(fh1vfs ../tools/fh1vfs.cpp)
target_compile_options(fh1vfs PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1vfs PRIVATE fh1base)
