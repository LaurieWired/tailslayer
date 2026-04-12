add_executable(
    hedged_read_cpp
    app_config.cpp
    app_config.hpp
    benchmark.cpp
    benchmark.hpp
    hw_utils.hpp
    main.cpp
    stats.cpp
    stats.hpp
)
target_compile_definitions(hedged_read_cpp PRIVATE _GNU_SOURCE)
target_link_libraries(hedged_read_cpp PRIVATE Threads::Threads)
target_compile_features(hedged_read_cpp PRIVATE cxx_std_17)
