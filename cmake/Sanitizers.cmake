function(dbdiff_enable_sanitizers target)
  if(NOT DBDIFF_ENABLE_SANITIZERS)
    return()
  endif()

  if(MSVC)
    message(FATAL_ERROR "DBDIFF_ENABLE_SANITIZERS is not supported with MSVC")
  endif()

  target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endfunction()

