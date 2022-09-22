# Prevent double-definition if two projects use this script
IF (NOT ParseArguments_INCLUDED)
  SET (ParseArguments_INCLUDED 1)

  # Utility macro to parse CMake-style argument lists. Arguments:
  # prefix - a common prefix for variables set by this function (see below).
  # arg_names - arguments that may take multiple values.
  # onearg_names - arguments that may take a single value.
  # option_names - arguments that take zero values.
  #
  # For each argument specified, this function will set a variable named
  # ${prefix}_${argname}. The value of this variable will be:
  #
  #  - a list of all specified values (arg_names)
  #  - the single value (onearg_names)
  #  - 1 or 0, depending on whether the option was specified (option_names)
  #
  # This macro has no concept of "required" or "optional" arguments; that logic
  # must be implemented by the calling code.
  MACRO(PARSE_ARGUMENTS prefix arg_names onearg_names option_names)
    SET(DEFAULT_ARGS)
    FOREACH(arg_name ${arg_names} ${onearg_names})
      SET(${prefix}_${arg_name})
    ENDFOREACH(arg_name)
    FOREACH(option ${option_names})
      SET(${prefix}_${option} 0)
    ENDFOREACH(option)

    SET(current_arg_name DEFAULT_ARGS)
    SET(current_arg_list)
    SET(current_arg_is_singular)
    SET(larg_names ${arg_names})
    SET(lonearg_names ${onearg_names})
    SET(loption_names ${option_names})
    FOREACH(arg ${ARGN})
      LIST(FIND larg_names "${arg}" is_arg_name)
      LIST(FIND lonearg_names "${arg}" is_onearg_name)
      IF (is_arg_name GREATER -1 OR is_onearg_name GREATER -1)
        SET(${prefix}_${current_arg_name} ${current_arg_list})
        SET(current_arg_name ${arg})
        SET(current_arg_list)
        IF (is_arg_name GREATER -1)
          SET(current_arg_is_singular)
        ELSE (is_arg_name GREATER -1)
          SET(current_arg_is_singular 1)
        ENDIF (is_arg_name GREATER -1)
      ELSE (is_arg_name GREATER -1 OR is_onearg_name GREATER -1)
        LIST(FIND loption_names "${arg}" is_option)
        IF (is_option GREATER -1)
          SET(${prefix}_${arg} 1)
        ELSE (is_option GREATER -1)
          SET(current_arg_list ${current_arg_list} ${arg})
          IF (current_arg_is_singular)
            LIST(LENGTH current_arg_list current_list_len)
            IF (current_list_len GREATER 1)
              MESSAGE (FATAL_ERROR "Argument ${current_arg_name} may only have one value")
            ENDIF (current_list_len GREATER 1)
          ENDIF (current_arg_is_singular)
        ENDIF (is_option GREATER -1)
      ENDIF (is_arg_name GREATER -1 OR is_onearg_name GREATER -1)
    ENDFOREACH(arg)
    SET(${prefix}_${current_arg_name} ${current_arg_list})
  ENDMACRO(PARSE_ARGUMENTS)

ENDIF (NOT ParseArguments_INCLUDED)
