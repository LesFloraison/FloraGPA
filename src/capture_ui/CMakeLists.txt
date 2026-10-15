find_package(Qt6 6.11 REQUIRED COMPONENTS Core Widgets)
set(CMAKE_AUTOMOC ON)
add_library(FloraCaptureUi CaptureClient.cpp CaptureClient.h CaptureDialog.cpp CaptureDialog.h AutoDetectClient.cpp AutoDetectClient.h)
target_include_directories(FloraCaptureUi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(FloraCaptureUi PUBLIC Qt6::Widgets)
if(BUILD_TESTING)
  find_package(Qt6 6.11 REQUIRED COMPONENTS Test)
  add_executable(FloraCaptureMock ../../tests/CaptureMock.cpp)
  target_link_libraries(FloraCaptureMock PRIVATE Qt6::Core)
  add_executable(FloraCaptureUiTests ../../tests/CaptureUiTests.cpp)
  target_link_libraries(FloraCaptureUiTests PRIVATE FloraCaptureUi Qt6::Test)
  add_dependencies(FloraCaptureUiTests FloraCaptureMock)
  add_test(NAME capture_ui COMMAND FloraCaptureUiTests -o "${CMAKE_CURRENT_BINARY_DIR}/capture-ui-$<CONFIG>.txt,txt")
  set_tests_properties(capture_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
  if(TARGET FloraCapture)
    add_dependencies(FloraCaptureUiTests FloraCapture FloraCaptureRuntime FloraCaptureFactoryProbe FloraCaptureLiveTarget)
    set_property(TEST capture_ui APPEND PROPERTY ENVIRONMENT
      "FLORA_NATIVE_CAPTURE_CONTROLLER=$<TARGET_FILE:FloraCapture>"
      "FLORA_NATIVE_CAPTURE_PROBE=$<TARGET_FILE:FloraCaptureFactoryProbe>"
      "FLORA_NATIVE_LIVE_TARGET=$<TARGET_FILE:FloraCaptureLiveTarget>")
    set_tests_properties(capture_ui PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()
