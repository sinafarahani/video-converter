# Development harness for the engine. Not part of the shipped app.
find_package(fmt CONFIG REQUIRED)

add_executable(convctl main.cpp)
target_link_libraries(convctl PRIVATE conv_engine fmt::fmt)
target_compile_features(convctl PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(convctl PRIVATE /utf-8)
endif()

set_target_properties(convctl PROPERTIES FOLDER "converter/tools")
