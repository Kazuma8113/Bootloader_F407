# ====== 添加 Hardware 目录支持 ======

# 自动递归扫描 Hardware 下所有 .c 源文件
file(GLOB_RECURSE HARDWARE_SOURCES
        ${CMAKE_SOURCE_DIR}/Hardware/*.c
)

# 自动递归扫描 Hardware 下所有 .h 头文件（IDE 可见，编译非必须）
file(GLOB_RECURSE HARDWARE_HEADERS
        ${CMAKE_SOURCE_DIR}/Hardware/*.h
)

# Hardware 根目录本身加入 include path
target_include_directories(${PROJECT_NAME}.elf PRIVATE ${CMAKE_SOURCE_DIR}/Hardware)

# 递归搜索所有子目录作为 include path
file(GLOB_RECURSE HARDWARE_SUBDIRS
        LIST_DIRECTORIES true
        ${CMAKE_SOURCE_DIR}/Hardware/*
)

foreach(subdir ${HARDWARE_SUBDIRS})
    if(IS_DIRECTORY ${subdir})
        target_include_directories(${PROJECT_NAME}.elf PRIVATE ${subdir})
    endif()
endforeach()

# 将所有 .c 源文件加入编译目标
target_sources(${PROJECT_NAME}.elf PRIVATE ${HARDWARE_SOURCES})

# 将 .h 也加入，方便 IDE 项目树显示
target_sources(${PROJECT_NAME}.elf PRIVATE ${HARDWARE_HEADERS})
