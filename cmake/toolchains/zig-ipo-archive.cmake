# Included after each project() via CMAKE_PROJECT_INCLUDE so that it runs after
# CMake's Clang module has selected its compiler-ar based IPO archive rules.
# This is also the first point after project() where CMP0137 can be enabled for
# CheckIPOSupported's whole-project try_compile.
if(POLICY CMP0137)
    cmake_policy(SET CMP0137 NEW)
endif()
foreach(_GP32_ZIG_LANGUAGE IN ITEMS C CXX)
    foreach(_GP32_ZIG_RULE IN ITEMS CREATE APPEND FINISH)
        set(CMAKE_${_GP32_ZIG_LANGUAGE}_ARCHIVE_${_GP32_ZIG_RULE}_IPO
            "${CMAKE_${_GP32_ZIG_LANGUAGE}_ARCHIVE_${_GP32_ZIG_RULE}}")
    endforeach()
endforeach()
unset(_GP32_ZIG_LANGUAGE)
unset(_GP32_ZIG_RULE)
foreach(_GP32_ZIG_PROJECT_HOOK IN LISTS _GP32_ZIG_PREVIOUS_PROJECT_INCLUDE)
    include("${_GP32_ZIG_PROJECT_HOOK}")
endforeach()
unset(_GP32_ZIG_PROJECT_HOOK)
