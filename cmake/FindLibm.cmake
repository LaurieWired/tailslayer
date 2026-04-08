include(CheckCSourceCompiles)
include(FindPackageHandleStandardArgs)

set(libm_test_program "
#include <math.h>
int main() {
    double x = sin(1.0);
    return 0;
}")
check_c_source_compiles("${libm_test_program}" HAVE_MATH_WITHOUT_LIBM)

if (HAVE_MATH_WITHOUT_LIBM)
    set(found_reason "(included in stdlib)")
    find_package_handle_standard_args(Libm REQUIRED_VARS found_reason)
    if (Libm_FOUND AND NOT TARGET libm::libm)
        add_library(libm::libm INTERFACE IMPORTED)
    endif ()
else ()
    find_library(LIBM_LIBRARY m)
    if (LIBM_LIBRARY)
        set(CMAKE_REQUIRED_LIBRARIES "${LIBM_LIBRARY}")
        check_c_source_compiles("${libm_test_program}" HAVE_MATH_WITH_LIBM)
        unset(CMAKE_REQUIRED_LIBRARIES)
    endif ()

    find_package_handle_standard_args(Libm REQUIRED_VARS LIBM_LIBRARY HAVE_MATH_WITH_LIBM)
    if (Libm_FOUND AND NOT TARGET libm::libm)
        add_library(libm::libm UNKNOWN IMPORTED)
        set_target_properties(libm::libm PROPERTIES IMPORTED_LOCATION "${LIBM_LIBRARY}")
    endif ()
endif ()
