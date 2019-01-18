
ign_add_component(register INTERFACE
  DEPENDS_ON_COMPONENTS loader
  GET_TARGET_NAME register)

target_link_libraries(${register}
  INTERFACE
    ${PROJECT_LIBRARY_TARGET_NAME}-loader)

install(
  DIRECTORY include/
  DESTINATION ${IGN_INCLUDE_INSTALL_DIR_FULL})
