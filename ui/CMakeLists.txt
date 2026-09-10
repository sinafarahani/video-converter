# Builds the Vue production bundle into ui/dist.
#
# Kept as a custom target rather than something clever so that a developer can
# always fall back to running `npm install && npm run build` here by hand.

find_program(NPM_EXECUTABLE NAMES npm.cmd npm
             DOC "npm, used to build the Vue frontend")

if(NOT NPM_EXECUTABLE)
  message(FATAL_ERROR
    "npm was not found, so the Vue UI cannot be built.\n"
    "Install Node.js, or configure with -DCONVERTER_BUILD_UI=OFF and build "
    "ui/dist yourself.")
endif()

set(UI_SRC "${CMAKE_CURRENT_SOURCE_DIR}")
set(UI_DIST "${UI_SRC}/dist/index.html")

# Only src/ is globbed recursively. Listing index.html under GLOB_RECURSE would
# also match dist/index.html and node_modules/**/index.html -- and since
# dist/index.html is this command's own OUTPUT, that produced a dependency cycle.
file(GLOB_RECURSE UI_SRC_FILES CONFIGURE_DEPENDS "${UI_SRC}/src/*")

set(UI_SOURCES
  ${UI_SRC_FILES}
  "${UI_SRC}/index.html"
  "${UI_SRC}/package.json"
  "${UI_SRC}/vite.config.ts")

# `npm ci` when there is a lockfile, `npm install` otherwise. Touching a stamp
# afterwards keeps CMake from reinstalling on every build.
set(UI_STAMP "${CMAKE_CURRENT_BINARY_DIR}/npm-install.stamp")
add_custom_command(
  OUTPUT "${UI_STAMP}"
  COMMAND ${CMAKE_COMMAND} -E echo "Installing UI dependencies..."
  COMMAND "${NPM_EXECUTABLE}" install --no-fund --no-audit
  COMMAND ${CMAKE_COMMAND} -E touch "${UI_STAMP}"
  WORKING_DIRECTORY "${UI_SRC}"
  DEPENDS "${UI_SRC}/package.json"
  COMMENT "npm install"
  VERBATIM)

add_custom_command(
  OUTPUT "${UI_DIST}"
  COMMAND "${NPM_EXECUTABLE}" run build
  WORKING_DIRECTORY "${UI_SRC}"
  DEPENDS "${UI_STAMP}" ${UI_SOURCES}
  COMMENT "Building the Vue UI"
  VERBATIM)

add_custom_target(converter_ui DEPENDS "${UI_DIST}")
set_target_properties(converter_ui PROPERTIES FOLDER "converter")
