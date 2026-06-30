# deploy_copy.cmake — copy a file, but don't fail the build if the copy
# fails (e.g. the game is running and the DLL is locked).
get_filename_component(dest_dir "${TO}" DIRECTORY)
execute_process(
    COMMAND ${CMAKE_COMMAND} -E make_directory "${dest_dir}"
)
execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FROM}" "${TO}"
    RESULT_VARIABLE result
)
if(result EQUAL 0)
    message(STATUS "Deployed ${FROM} -> ${TO}")
else()
    message(WARNING "Could not copy ${FROM} -> ${TO} (rc=${result}). "
                    "Is the game running? Close it and rebuild, or copy manually.")
endif()
