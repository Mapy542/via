cmake_minimum_required(VERSION 3.20)

set(VIA_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/VERSION")
if(NOT EXISTS "${VIA_VERSION_FILE}")
    message(FATAL_ERROR "Missing VERSION file at ${VIA_VERSION_FILE}")
endif()

file(STRINGS "${VIA_VERSION_FILE}" VIA_CHECKED_IN_VERSION LIMIT_COUNT 1)
string(STRIP "${VIA_CHECKED_IN_VERSION}" VIA_CHECKED_IN_VERSION)

set(VIA_VERSION_OVERRIDE "" CACHE STRING "Override Via application version for one-off builds")

if(VIA_VERSION_OVERRIDE)
    string(STRIP "${VIA_VERSION_OVERRIDE}" VIA_RESOLVED_VERSION)
else()
    set(VIA_RESOLVED_VERSION "${VIA_CHECKED_IN_VERSION}")
endif()

if(VIA_RESOLVED_VERSION STREQUAL "")
    message(FATAL_ERROR "Via version cannot be empty")
endif()

if(NOT VIA_RESOLVED_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
    message(FATAL_ERROR
        "Via version '${VIA_RESOLVED_VERSION}' is invalid. Expected 3 or 4 numeric components."
    )
endif()

project(via
    VERSION ${VIA_RESOLVED_VERSION}
    DESCRIPTION "Google Drive Desktop Client for Linux"
    LANGUAGES CXX
)

set(VIA_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${VIA_GENERATED_DIR}")
file(GENERATE OUTPUT "${VIA_GENERATED_DIR}/ViaVersion.h"
    CONTENT "#pragma once\n#define VIA_APP_VERSION \"${PROJECT_VERSION}\"\n"
)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/via-version.txt" CONTENT "${PROJECT_VERSION}\n")

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
    src/ui/UiStatusCoordinator.cpp
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
    src/sync/MirrorSyncController.cpp
    src/sync/MirrorSyncRuntime.cpp
    src/sync/ChangeProcessor.cpp
    src/sync/SyncActionQueue.cpp
    src/sync/SyncActionThread.cpp
    src/sync/RuntimePauseController.cpp
    src/sync/MirrorPathResolver.cpp
    src/sync/FileFilter.cpp
    src/sync/FullSync.cpp
    src/sync/SyncSettings.cpp
    src/sync/TrashPolicy.cpp
    src/fuse/FuseDriver.cpp
    src/fuse/FileCache.cpp
    src/fuse/DirtySyncWorker.cpp
    src/fuse/MetadataCache.cpp
    src/fuse/MetadataRefreshWorker.cpp
    src/utils/NotificationManager.cpp
    src/utils/LogManager.cpp
    src/utils/CacheMaintenance.cpp
    src/utils/FileInUseChecker.cpp
    src/utils/PowerProfileMonitor.cpp
    src/utils/SuspendMonitor.cpp
)

# Header files
set(HEADERS
    src/ui/MainWindow.h
    src/ui/SettingsWindow.h
    src/ui/SystemTrayManager.h
    src/ui/UiStatusCoordinator.h
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
    src/sync/MirrorSyncController.h
    src/sync/MirrorSyncRuntime.h
    src/sync/ChangeProcessor.h
    src/sync/SyncActionQueue.h
    src/sync/SyncActionThread.h
    src/sync/RuntimePauseController.h
    src/sync/MirrorPathResolver.h
    src/sync/FileFilter.h
    src/sync/FullSync.h
    src/sync/SyncSettings.h
    src/sync/TrashPolicy.h
    src/fuse/FuseDriver.h
    src/fuse/FileCache.h
    src/fuse/DirtySyncWorker.h
    src/fuse/MetadataCache.h
    src/fuse/MetadataRefreshWorker.h
    src/utils/NotificationManager.h
    src/utils/LogManager.h
    src/utils/CacheMaintenance.h
    src/utils/FileInUseChecker.h
    src/utils/PowerProfileMonitor.h
    src/utils/AutostartManager.h
    src/utils/NativeDocShortcutHandler.h
    src/utils/UpdateChecker.h
    src/utils/SuspendMonitor.h
)

# Resource files
set(RESOURCES
    res/resources.qrc
)

set(VIA_THEME_APP_ICON res/icons/via.svg)
set(VIA_THEME_MIMETYPE_ICON_FILES
    res/icons/mimetypes/application-x-via-gdoc.svg
    res/icons/mimetypes/application-x-via-gsheet.svg
    res/icons/mimetypes/application-x-via-gslides.svg
    res/icons/mimetypes/application-x-via-gdraw.svg
    res/icons/mimetypes/application-x-via-gdrive.svg
)
set(VIA_THEME_ICON_FILES
    ${VIA_THEME_APP_ICON}
    ${VIA_THEME_MIMETYPE_ICON_FILES}
)

foreach(icon_file IN LISTS VIA_THEME_ICON_FILES)
    get_filename_component(icon_dir "${icon_file}" DIRECTORY)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/${icon_dir}")
    configure_file(
        "${CMAKE_SOURCE_DIR}/${icon_file}"
        "${CMAKE_BINARY_DIR}/${icon_file}"
        COPYONLY
    )
endforeach()

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
    ${VIA_GENERATED_DIR}
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

install(FILES res/via-native-docs.xml
    DESTINATION share/mime/packages
)

install(FILES ${VIA_THEME_APP_ICON}
    DESTINATION share/icons/hicolor/scalable/apps
)

install(FILES
    ${VIA_THEME_MIMETYPE_ICON_FILES}
    DESTINATION share/icons/hicolor/scalable/mimetypes
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
        ${VIA_GENERATED_DIR}
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
    add_qt_test(test_RuntimePauseController tests/sync/TestRuntimePauseController.cpp)
    add_qt_test(test_PowerProfileMonitor tests/sync/TestPowerProfileMonitor.cpp)
    add_qt_test(test_LocalChangeWatcher tests/sync/TestLocalChangeWatcher.cpp)
    add_qt_test(test_RemoteChangeWatcher tests/sync/TestRemoteChangeWatcher.cpp)
    add_qt_test(test_FullSync tests/sync/TestFullSync.cpp)
    add_qt_test(test_TrashPolicy tests/sync/TestTrashPolicy.cpp)

    # FUSE subsystem tests
    add_qt_test(test_FileCache tests/fuse/TestFileCache.cpp)
    add_qt_test(test_CacheMaintenance tests/fuse/TestCacheMaintenance.cpp)
    add_qt_test(test_DirtySyncWorker tests/fuse/TestDirtySyncWorker.cpp)
    add_qt_test(test_FuseDriverLifecycle tests/fuse/TestFuseDriverLifecycle.cpp)
    add_qt_test(test_MetadataCache tests/fuse/TestMetadataCache.cpp)
    add_qt_test(test_MetadataRefreshWorker tests/fuse/TestMetadataRefreshWorker.cpp)
    add_qt_test(test_NativeDocShortcutHandler tests/fuse/TestNativeDocShortcutHandler.cpp)

    # Utility tests
    add_qt_test(test_PathUtils tests/utils/TestPathUtils.cpp)

    # UI tests
    add_qt_test(test_AutostartManager tests/ui/TestAutostartManager.cpp)
    add_qt_test(test_LogManager tests/utils/TestLogManager.cpp)
    add_qt_test(test_SettingsWindow tests/ui/TestSettingsWindow.cpp)
    add_qt_test(test_UiStatusCoordinator tests/ui/TestUiStatusCoordinator.cpp)
    add_qt_test(test_UiStatusIntegration tests/ui/TestUiStatusIntegration.cpp)

    # Auth subsystem tests
    add_qt_test(test_TokenRefresh tests/auth/TestTokenRefresh.cpp)
    add_qt_test(test_AccountSwitch tests/auth/TestAccountSwitch.cpp)
    add_qt_test(test_TokenStorage tests/auth/TestTokenStorage.cpp)

    # API tests
    add_qt_test(test_GoogleDriveClient tests/api/TestGoogleDriveClient.cpp)

    # ==================================================================
    # E2E FUSE Test Harness (headless mount for pjdfstest / fsstress)
    # ==================================================================
    add_executable(via_fuse_harness tests/e2e/FuseTestHarness.cpp)
    target_link_libraries(via_fuse_harness PRIVATE
        via_testable
    )

endif()
