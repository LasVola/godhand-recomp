# cmake -DSRC=<folder> -DDST=<folder> -P copy_if_missing.cmake
# Copies the files of SRC into DST only where DST has no file of that name (a player's own shader cache, which grows
# as they play, is never replaced by the shipped one).
file(GLOB files RELATIVE "${SRC}" "${SRC}/*")
file(MAKE_DIRECTORY "${DST}")
foreach(name IN LISTS files)
    if(NOT IS_DIRECTORY "${SRC}/${name}" AND NOT EXISTS "${DST}/${name}")
        file(COPY "${SRC}/${name}" DESTINATION "${DST}")
    endif()
endforeach()
