cmake_minimum_required(VERSION 3.25)
project(opendisplay-win CXX)

include(CTest)

if(NOT WIN32)
  message(FATAL_ERROR "opendisplay-win only builds on Windows (Media Foundation, DXGI, Win32).")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so a released .exe runs on a bare Windows box without the VC++
# Redistributable installed. Costs ~250 KB; the UCRT part (api-ms-win-crt-*)
# ships with Windows 10/11 either way.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(opendisplay-win
  src/main.cpp
  src/net/Connection.cpp
  src/net/UsbMux.cpp
  src/net/Protocol.cpp
  src/net/Mdns.cpp
  src/net/NetworkSafety.cpp
  src/encode/AnnexB.cpp
  src/encode/H264Encoder.cpp
  src/display/VirtualDisplay.cpp
  src/display/DesktopDuplication.cpp
  src/display/DisplayCatalog.cpp
  src/input/InputInjector.cpp
  src/app/Log.cpp
  src/app/WindowRouting.cpp
  src/app/TaskbarRouter.cpp
  src/app/UiModel.cpp
  src/app/DisplayLauncher.cpp
  src/app/DeviceCoordinator.cpp
  src/app/SenderApp.cpp
  src/app/Config.cpp
  src/app/TrayApp.cpp
  src/app/resources.rc
)

target_include_directories(opendisplay-win PRIVATE
  src
  third_party/parsec-vdd
)

target_link_libraries(opendisplay-win PRIVATE
  ws2_32
  user32
  d3d11
  dxgi
  dxguid
  mfplat
  mf
  mfreadwrite
  mfuuid
  setupapi
  cfgmgr32
  shell32
  gdi32
  msimg32
  dwmapi
  ole32
  iphlpapi
)

target_compile_definitions(opendisplay-win PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

target_compile_options(opendisplay-win PRIVATE /W4 /permissive- /utf-8)

set_target_properties(opendisplay-win PROPERTIES
  OUTPUT_NAME "iPad互联"
)

if(BUILD_TESTING)
  add_executable(protocol-tests
    tests/protocol_tests.cpp
    src/net/Protocol.cpp
  )
  target_include_directories(protocol-tests PRIVATE src)
  target_compile_options(protocol-tests PRIVATE /W4 /permissive- /utf-8)
  add_test(NAME protocol-v3 COMMAND protocol-tests)

  add_executable(ui-model-tests
    tests/ui_model_tests.cpp
    src/app/Config.cpp
    src/app/UiModel.cpp
  )
  target_include_directories(ui-model-tests PRIVATE src)
  target_compile_options(ui-model-tests PRIVATE /W4 /permissive- /utf-8)
  add_test(NAME ui-model COMMAND ui-model-tests)

  add_executable(device-flow-tests
    tests/device_flow_tests.cpp
    src/app/Config.cpp
    src/app/DeviceCoordinator.cpp
    src/net/Mdns.cpp
    src/net/NetworkSafety.cpp
  )
  target_include_directories(device-flow-tests PRIVATE src)
  target_link_libraries(device-flow-tests PRIVATE ws2_32 iphlpapi ole32)
  target_compile_options(device-flow-tests PRIVATE /W4 /permissive- /utf-8)
  add_test(NAME device-flow COMMAND device-flow-tests)
endif()

# asInvoker (not requireAdministrator): the app runs un-elevated. The only
# operation that needs admin is registering a new iPad resolution as a
# parsec-vdd custom mode (one HKLM write); that path self-elevates a one-off
# (`--register-resolution`). Running the whole app elevated is optional (tray
# menu) — only needed to inject touch into elevated windows.
#
# GUI subsystem so the tray app doesn't flash a console window, but keep
# mainCRTStartup as the entry point so we still have a normal main(argc, argv)
# (populated from the command line) for the headless CLI mode.
target_link_options(opendisplay-win PRIVATE
  "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
  "/SUBSYSTEM:WINDOWS"
  "/ENTRY:mainCRTStartup"
)
