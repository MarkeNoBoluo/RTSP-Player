cmake_minimum_required(VERSION 3.16)
project(RTSP-Player LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 REQUIRED COMPONENTS Widgets OpenGL Multimedia)

set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rd/FFmpeg4.2.9")
set(FFMPEG_INCLUDE_DIRS ${FFMPEG_ROOT}/include)
set(FFMPEG_LIBRARY_DIRS ${FFMPEG_ROOT}/lib)

find_library(AVFORMAT_LIBRARY  avformat  PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
find_library(AVCODEC_LIBRARY   avcodec   PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
find_library(AVUTIL_LIBRARY    avutil    PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)

set(SOURCES
    src/main.cpp
    src/RTSPlayer.cpp
    src/StreamLifecycleManager.cpp
    src/PlayerStateMachine.cpp
    src/DemuxThread.cpp
    src/VideoDecodeThread.cpp
    src/RenderScheduler.cpp
    src/AudioWorker.cpp
    src/AudioPullDevice.cpp
    src/PacketQueue.cpp
    src/VideoFrameQueue.cpp
    src/AVClock.cpp
    src/GLVideoWidget.cpp
    src/PlayerStats.cpp
    src/logger/Logger.cpp
)

set(RESOURCES
    resources/shaders/shaders.qrc
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/MSVC2017_x64_$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/MSVC2017_x64_$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/MSVC2017_x64_$<CONFIG>")

add_executable(${PROJECT_NAME} ${SOURCES} ${RESOURCES})
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt5::Widgets
    Qt5::OpenGL
    Qt5::Multimedia
    ${AVFORMAT_LIBRARY}
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
)

# Copy FFmpeg DLLs to output directory for runtime
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FFMPEG_ROOT}/bin/avcodec-58.dll"
    "${FFMPEG_ROOT}/bin/avformat-58.dll"
    "${FFMPEG_ROOT}/bin/avutil-56.dll"
    "${FFMPEG_ROOT}/bin/swresample-3.dll"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
)
