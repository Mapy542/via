cmake_minimum_required(VERSION 3.20)

project(via
    VERSION 1.0.0
    DESCRIPTION "Google Drive Desktop Client for Linux"
    LANGUAGES CXX
)

# C++20 Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set Qt auto-tools
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt6 components
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    NetworkAuth
    Sql
    DBus
    Concurrent
    Test
)

# Find QtKeychain (OS keyring integration)
find_package(Qt6Keychain REQUIRED)

# Find FUSE3
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)

# Source files
set(SOURCES
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/SettingsWindow.cpp
    src/ui/SystemTrayManager.cpp
    src/ui/ConflictDialog.cpp
    src/auth/GoogleAuthManager.cpp
    src/auth/TokenStorage.cpp
    src/api/GoogleDriveClient.cpp
    src/api/DriveChange.cpp
    src/api/DriveFile.cpp
    src/sync/SyncDatabase.cpp
    src/sync/ChangeQueue.cpp
    src/sync/LocalChangeWatcher.cpp
    src/sync/RemoteChangeWatcher.cpp
    src/sync/ChangeProcessor.cpp
    src/sync/SyncActionQueue.cpp
    src/sync/SyncActionThread.cpp
    src/sync/FileFilter.cpp
    src/sync/FullSync.cpp
    src/sync/SyncSettings.cpp
    src/fuse/FuseDriver.cpp
    src/fuse/FileCache.cpp
    src/fuse/DirtySyncWorker.cpp
    src/fuse/MetadataCache.cpp
    src/fuse/MetadataRefreshWorker.cpp
    src/utils/NotificationManager.cpp
    src/utils/LogManager.cpp
    src/utils/FileInUseChecker.cpp
)

# Header files
set(HEADERS
    src/ui/MainWindow.h
    src/ui/SettingsWindow.h
    src/ui/SystemTrayManager.h
    src/ui/ConflictDialog.h
    src/auth/GoogleAuthManager.h
    src/auth/TokenStorage.h
    src/api/GoogleDriveClient.h
    src/api/DriveChange.h
    src/api/DriveFile.h
    src/sync/SyncDatabase.h
    src/sync/ChangeQueue.h
    src/sync/LocalChangeWatcher.h
    src/sync/RemoteChangeWatcher.h
    src/sync/ChangeProcessor.h
    src/sync/SyncActionQueue.h
    src/sync/SyncActionThread.h
    src/sync/FileFilter.h
    src/sync/FullSync.h
    src/sync/SyncSettings.h
    src/fuse/FuseDriver.h
    src/fuse/FileCache.h
    src/fuse/DirtySyncWorker.h
    src/fuse/MetadataCache.h
    src/fuse/MetadataRefreshWorker.h
    src/utils/NotificationManager.h
    src/utils/LogManager.h
    src/utils/FileInUseChecker.h
    src/utils/AutostartManager.h
    src/utils/UpdateChecker.h
)

# Resource files
set(RESOURCES
    res/resources.qrc
)

# Create executable
add_executable(${PROJECT_NAME}
    ${SOURCES}
    ${HEADERS}
    ${RESOURCES}
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::NetworkAuth
    Qt6::Sql
    Qt6::DBus
    Qt6::Concurrent
    PkgConfig::FUSE3
    qt6keychain
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Compiler warnings
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Install targets
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

install(FILES res/via.desktop
    DESTINATION share/applications
)

install(FILES res/icons/via.svg
    DESTINATION share/icons/hicolor/scalable/apps
)

# ==============================================================================
# Testing Support (Qt Test + CTest)
# ==============================================================================
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    include(CTest)

    configure_file(
        ${CMAKE_SOURCE_DIR}/CTestCustom.cmake
        ${CMAKE_BINARY_DIR}/CTestCustom.cmake
        COPYONLY
    )


    # Create a static library for testable code
    add_library(via_testable STATIC ${SOURCES} ${HEADERS})
    target_link_libraries(via_testable PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::NetworkAuth
        Qt6::Sql
        Qt6::DBus
        Qt6::Concurrent
        Qt6::Widgets
        Qt6::Test
        PkgConfig::FUSE3
        qt6keychain
    )
    target_include_directories(via_testable PUBLIC
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_options(via_testable PRIVATE -Wall -Wextra)

    # Macro to add a Qt test
    macro(add_qt_test TEST_NAME TEST_SOURCE)
        add_executable(${TEST_NAME} ${TEST_SOURCE})
        target_link_libraries(${TEST_NAME} PRIVATE
            via_testable
            Qt6::Test
        )
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
        set_tests_properties(${TEST_NAME} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endmacro()

    # Sync subsystem tests
    add_qt_test(test_ChangeQueue tests/sync/TestChangeQueue.cpp)
    add_qt_test(test_ChangeProcessor tests/sync/TestChangeProcessor.cpp)
    add_qt_test(test_SyncDatabase tests/sync/TestSyncDatabase.cpp)
    add_qt_test(test_SyncActionQueue tests/sync/TestSyncActionQueue.cpp)
    add_qt_test(test_SyncActionThread tests/sync/TestSyncActionThread.cpp)
    add_qt_test(test_LocalChangeWatcher tests/sync/TestLocalChangeWatcher.cpp)
    add_qt_test(test_RemoteChangeWatcher tests/sync/TestRemoteChangeWatcher.cpp)
    add_qt_test(test_FullSync tests/sync/TestFullSync.cpp)

    # FUSE subsystem tests
    add_qt_test(test_FileCache tests/fuse/TestFileCache.cpp)
    add_qt_test(test_DirtySyncWorker tests/fuse/TestDirtySyncWorker.cpp)
    add_qt_test(test_MetadataCache tests/fuse/TestMetadataCache.cpp)

    # Auth subsystem tests
    add_qt_test(test_TokenRefresh tests/auth/TestTokenRefresh.cpp)
    add_qt_test(test_AccountSwitch tests/auth/TestAccountSwitch.cpp)

endif()
