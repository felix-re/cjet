FUNCTION(GENERATE_VERSION VERSION_FILE)
  
  FILE(STRINGS ${VERSION_FILE} VERSION_STRING)
  SET(CJET_VERSION ${VERSION_STRING} PARENT_SCOPE)

  EXECUTE_PROCESS(
    COMMAND git diff --shortstat
    COMMAND tail -n1
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DIRTY
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  EXECUTE_PROCESS(
  	COMMAND git describe --exact-match --tags HEAD
  	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	RESULT_VARIABLE IS_TAG
  	OUTPUT_STRIP_TRAILING_WHITESPACE
  	ERROR_QUIET
  )

  EXECUTE_PROCESS(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  IF((IS_TAG EQUAL 0) AND (GIT_DIRTY STREQUAL ""))
  	SET(${PROJECTNAME}_VERSION_TWEAK "")
  ELSE()
  	EXECUTE_PROCESS(
  	  COMMAND git rev-list HEAD --count
  	  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  	  OUTPUT_VARIABLE GIT_REV_COUNT
  	  OUTPUT_STRIP_TRAILING_WHITESPACE
  	  ERROR_QUIET
  	)
  	IF(GIT_REV_COUNT STREQUAL "")
  		SET(${PROJECTNAME}_VERSION_TWEAK "-unknown")
  	ELSE()
  		SET(${PROJECTNAME}_VERSION_TWEAK "-${GIT_REV_COUNT}")
  	ENDIF()
  ENDIF()

  
  IF(GIT_DIRTY STREQUAL "")
  	SET(${PROJECTNAME}_VERSION_DIRTY "")
  ELSE()
  	SET(${PROJECTNAME}_VERSION_DIRTY ".dirty")
  ENDIF()

  SET(${PROJECTNAME}_BUILDINFO "+${GIT_HASH}${${PROJECTNAME}_VERSION_DIRTY}")

  SET(CJET_LAST ${${PROJECTNAME}_VERSION_TWEAK}${${PROJECTNAME}_BUILDINFO} PARENT_SCOPE)

ENDFUNCTION(GENERATE_VERSION)

