cmake_minimum_required( VERSION 3.5 )

project( video-split-black )

find_package( PkgConfig REQUIRED )
pkg_check_modules( ffmpeg REQUIRED IMPORTED_TARGET
    libavdevice
    libavfilter
    libavformat
    libavcodec
    libswresample
    libswscale
    libavutil
)

add_executable( ffmpeg-split-black main.c kstring.c )
target_include_directories( ffmpeg-split-black PRIVATE ${ffmpeg_INCLUDE_DIRS} )
target_link_libraries( ffmpeg-split-black PRIVATE PkgConfig::ffmpeg )
