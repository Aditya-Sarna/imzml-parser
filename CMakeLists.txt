cmake_minimum_required(VERSION 3.18)
project(imzml_parser
    VERSION 1.0.0
    DESCRIPTION "imzML parser - SAX-based, OpenMS-architecture"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# C++ standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Build type default
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        -Wno-missing-field-initializers
    )
elseif(MSVC)
    add_compile_options(/W4 /WX-)
endif()

# ---------------------------------------------------------------------------
# ccache — transparent compiler-output cache
#   Install: brew install ccache  (macOS)  |  apt install ccache  (Ubuntu)
#
# Usage is automatic when ccache is on PATH.  Override with:
#   cmake -DUSE_CCACHE=OFF ..
# To inspect cache stats afterwards: ccache --show-stats
# Cache location: $CCACHE_DIR  (default ~/.ccache on macOS)
# ---------------------------------------------------------------------------
option(USE_CCACHE "Use ccache to speed up recompilation" ON)

if(USE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        message(STATUS "  ccache found: ${CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
        # On macOS, clang modules can interfere — disable the PCH cache
        if(APPLE)
            set(ENV{CCACHE_CPP2} "true")
        endif()
    else()
        message(STATUS "  ccache not found — building without cache")
    endif()
endif()

# ---------------------------------------------------------------------------
# Find expat (SAX parser dependency)
# Install on macOS:   brew install expat
# Install on Ubuntu:  apt-get install libexpat1-dev
# ---------------------------------------------------------------------------
find_package(EXPAT REQUIRED)

# ---------------------------------------------------------------------------
# imzml_parser - static library
# ---------------------------------------------------------------------------
add_library(imzml_parser STATIC
    src/XMLHandler.cpp
    src/ImzMLHandler.cpp
    src/IBDReader.cpp
    src/ImzMLFile.cpp
    src/ImzMLWriter.cpp
)

target_include_directories(imzml_parser
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(imzml_parser
    PUBLIC
        EXPAT::EXPAT
)

# macOS: expat may be keg-only via brew; help the compiler find headers
if(APPLE)
    execute_process(
        COMMAND brew --prefix expat
        OUTPUT_VARIABLE BREW_EXPAT_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(BREW_EXPAT_PREFIX)
        target_include_directories(imzml_parser PUBLIC "${BREW_EXPAT_PREFIX}/include")
        target_link_directories(imzml_parser PUBLIC "${BREW_EXPAT_PREFIX}/lib")
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests executable
# ---------------------------------------------------------------------------
option(BUILD_TESTS "Build test binaries" ON)

if(BUILD_TESTS)
    enable_testing()

    # Data generator
    add_executable(gen_example_data tests/gen_example_data.cpp)
    target_include_directories(gen_example_data PRIVATE include)
    set_target_properties(gen_example_data PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
    )

    # Main test binary (continuous parser + writer round-trip + processed)
    add_executable(test_imzml tests/test_imzml.cpp)
    target_link_libraries(test_imzml PRIVATE imzml_parser)
    set_target_properties(test_imzml PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
    )

    # CTest integration
    add_test(
        NAME imzml_continuous_3x3
        COMMAND ${CMAKE_BINARY_DIR}/tests/test_imzml
                ${CMAKE_BINARY_DIR}/tests/data/Example_Continuous.imzML
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests
    )

    add_test(
        NAME imzml_writer_roundtrip
        COMMAND ${CMAKE_BINARY_DIR}/tests/test_imzml
                --writer-test
                ${CMAKE_BINARY_DIR}/tests/data/Example_Continuous.imzML
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests
    )

    add_test(
        NAME imzml_processed_roundtrip
        COMMAND ${CMAKE_BINARY_DIR}/tests/test_imzml
                --processed-test
                ${CMAKE_BINARY_DIR}/tests/data/Example_Continuous.imzML
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests
    )

    # Auto-generate test data before running tests
    add_custom_command(
        OUTPUT  ${CMAKE_BINARY_DIR}/tests/data/Example_Continuous.imzML
                ${CMAKE_BINARY_DIR}/tests/data/Example_Continuous.ibd
        COMMAND ${CMAKE_BINARY_DIR}/tests/gen_example_data
                ${CMAKE_BINARY_DIR}/tests/data
        DEPENDS gen_example_data
        COMMENT "Generating 3x3 continuous imzML example data"
    )

    add_custom_target(generate_test_data ALL
        DEPENDS
            ${CMAKE_BINARY_DIR}/tests/data/Example_Continuous.imzML
            ${CMAKE_BINARY_DIR}/tests/data/Example_Continuous.ibd
    )

    add_dependencies(test_imzml generate_test_data)

    # -----------------------------------------------------------------------
    # Large real-data test: AP-SMALDI mouse urinary bladder (S096)
    #
    # The .ibd file is ~837 MB and lives in a GitHub Release, not in the
    # repo tree (same pattern as OpenMS BrukerTIMS test data).
    #
    # To enable:
    #   cmake -DIMZML_DOWNLOAD_TEST_DATA=ON ..
    #
    # Release URL base:
    #   https://github.com/Aditya-Sarna/imzml-test-data/releases/download/v1.0.0/
    # -----------------------------------------------------------------------
    option(IMZML_DOWNLOAD_TEST_DATA
        "Download large real-data MSI files from GitHub Releases for integration tests"
        OFF)

    set(IMZML_TEST_DATA_BASE_URL
        "https://github.com/Aditya-Sarna/imzml-test-data/releases/download/v1.0.0"
        CACHE STRING "Base URL for large test dataset downloads")

    set(IMZML_TEST_DATA_DIR "${CMAKE_BINARY_DIR}/tests/data/real"
        CACHE PATH "Directory where downloaded real test data is stored")

    if(IMZML_DOWNLOAD_TEST_DATA)
        set(BLADDER_IMZML "${IMZML_TEST_DATA_DIR}/HR2MSImouseurinarybladderS096.imzML")
        set(BLADDER_IBD   "${IMZML_TEST_DATA_DIR}/HR2MSImouseurinarybladderS096.ibd")

        # Download .imzML (small XML metadata, always fast)
        if(NOT EXISTS "${BLADDER_IMZML}")
            message(STATUS "  Downloading bladder .imzML metadata...")
            file(DOWNLOAD
                "${IMZML_TEST_DATA_BASE_URL}/HR2MSImouseurinarybladderS096.imzML"
                "${BLADDER_IMZML}"
                SHOW_PROGRESS
                STATUS dl_status
            )
            list(GET dl_status 0 dl_code)
            if(NOT dl_code EQUAL 0)
                message(WARNING "  Failed to download .imzML — real-data test disabled")
                set(IMZML_DOWNLOAD_TEST_DATA OFF)
            endif()
        endif()

        # Download .ibd (large binary, ~837 MB — skipped if already present)
        if(IMZML_DOWNLOAD_TEST_DATA AND NOT EXISTS "${BLADDER_IBD}")
            message(STATUS "  Downloading bladder .ibd (~837 MB) — this may take a while...")
            file(DOWNLOAD
                "${IMZML_TEST_DATA_BASE_URL}/HR2MSImouseurinarybladderS096.ibd"
                "${BLADDER_IBD}"
                SHOW_PROGRESS
                STATUS dl_status
            )
            list(GET dl_status 0 dl_code)
            if(NOT dl_code EQUAL 0)
                message(WARNING "  Failed to download .ibd — real-data test disabled")
                file(REMOVE "${BLADDER_IBD}")
                set(IMZML_DOWNLOAD_TEST_DATA OFF)
            endif()
        endif()

        # Register CTest only if both files are present
        if(EXISTS "${BLADDER_IMZML}" AND EXISTS "${BLADDER_IBD}")
            add_test(
                NAME imzml_real_bladder
                COMMAND ${CMAKE_BINARY_DIR}/tests/test_imzml "${BLADDER_IMZML}"
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests
            )
            message(STATUS "  Real-data test: ENABLED (${BLADDER_IMZML})")
        else()
            message(STATUS "  Real-data test: DISABLED (data not downloaded)")
        endif()
    else()
        message(STATUS "  Real-data test: DISABLED (set -DIMZML_DOWNLOAD_TEST_DATA=ON to enable)")
    endif()
endif()

# ---------------------------------------------------------------------------
# OpenMS integration layer (Xerces-C SAX2 backend)
# Install on macOS:   brew install xerces-c
# Install on Ubuntu:  apt-get install libxerces-c-dev
# ---------------------------------------------------------------------------
option(BUILD_OPENMS_INTEGRATION "Build OpenMS-API integration layer" ON)

# Note: Xerces-C (Homebrew) is no longer a direct dependency of openms_imzml.
# All XML parsing is done inside OMLoader.cpp (bioconda TU) which uses bioconda
# Xerces 3.2 transitively via libOpenMS.dylib — no separate Xerces find needed.

if(BUILD_OPENMS_INTEGRATION)
    # ---------------------------------------------------------------------------
    # Bioconda OpenMS 3.5.0 — used for the real MzMLFile parser (Phase 1)
    #
    # We build OMLoader.cpp as a SEPARATE OBJECT LIBRARY with bioconda include
    # paths so that its Xerces 3.2 headers (pulled in transitively by MzMLFile.h)
    # never appear in the same compilation unit as Homebrew Xerces 3.3 headers.
    # ---------------------------------------------------------------------------
    set(OPENMS_CONDA_ENV "$ENV{HOME}/miniforge3/envs/openms_env"
        CACHE PATH "Path to bioconda openms_env conda environment")

    # Detect library extension (.dylib on macOS, .so on Linux)
    if(APPLE)
        set(OPENMS_LIB_NAME "libOpenMS.dylib")
    else()
        set(OPENMS_LIB_NAME "libOpenMS.so")
    endif()
    set(OPENMS_LIB "${OPENMS_CONDA_ENV}/lib/${OPENMS_LIB_NAME}")

    if(EXISTS "${OPENMS_LIB}")
        message(STATUS "  Bioconda libOpenMS: ${OPENMS_LIB}")

        # -----------------------------------------------------------------------
        # SHARED library for OMLoader — critical for ODR safety!
        #
        # Why shared (.dylib) and NOT static (.a) or OBJECT library?
        #
        # ODR violation if static: Our binary also defines OpenMS::MSSpectrum
        # (our local wrapper in libopenms_imzml.a). If OMLoader.cpp.o is linked
        # into the same binary (as an OBJECT or via static archive), the linker
        # uses our local MSSpectrum definition for ALL OpenMS::MSSpectrum symbols
        # — including destructor calls on bioconda-allocated PeakMap spectra.
        # That causes a crash (wrong memory layout).
        #
        # With a SHARED library, macOS two-level namespace resolver binds
        # libomloader.dylib's OpenMS:: references to libOpenMS.dylib at load
        # time — completely independent of our local MSSpectrum wrapper.
        # -----------------------------------------------------------------------
        add_library(omloader SHARED
            openms_integration/src/OMLoader.cpp
        )
        target_compile_features(omloader PRIVATE cxx_std_20)
        target_include_directories(omloader SYSTEM PRIVATE
            "${OPENMS_CONDA_ENV}/include"
            "${OPENMS_CONDA_ENV}/include/eigen3"
            "${OPENMS_CONDA_ENV}/include/qt6"
        )
        # OMLoader.h lives in include_bridge/ — separate from openms_integration/include/
        # so it does NOT shadow any bioconda OpenMS/* headers.
        target_include_directories(omloader PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/openms_integration/include_bridge"
        )
        # Boost for config.h → boost/current_function.hpp (macOS Homebrew only)
        if(APPLE)
            execute_process(
                COMMAND brew --prefix boost
                OUTPUT_VARIABLE BREW_BOOST_PREFIX_OBJ
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(BREW_BOOST_PREFIX_OBJ)
                target_include_directories(omloader SYSTEM PRIVATE "${BREW_BOOST_PREFIX_OBJ}/include")
            endif()
        endif()
        target_link_libraries(omloader PRIVATE OpenMS::OpenMS XercesC_bioconda)
        set_target_properties(omloader PROPERTIES
            INSTALL_RPATH "${OPENMS_CONDA_ENV}/lib"
            BUILD_RPATH   "${OPENMS_CONDA_ENV}/lib"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        )

        # Bioconda Xerces 3.2 — needed directly by loadImzMLFull() (XMLPlatformUtils)
        add_library(XercesC_bioconda SHARED IMPORTED)
        set_target_properties(XercesC_bioconda PROPERTIES
            IMPORTED_LOCATION "${OPENMS_CONDA_ENV}/lib/libxerces-c${CMAKE_SHARED_LIBRARY_SUFFIX}"
        )

        # Import libOpenMS as a CMake imported target
        add_library(OpenMS::OpenMS SHARED IMPORTED)
        set_target_properties(OpenMS::OpenMS PROPERTIES
            IMPORTED_LOCATION "${OPENMS_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENMS_CONDA_ENV}/include;${OPENMS_CONDA_ENV}/include/eigen3"
        )

        set(HAS_BIOCONDA_OPENMS TRUE)
    else()
        message(WARNING "  Bioconda libOpenMS not found at ${OPENMS_LIB} — OMLoader will be stub-only")
        set(HAS_BIOCONDA_OPENMS FALSE)
    endif()

    # --- Static library: openms_imzml ----------------------------------------
    # ImzMLHandler.cpp is compiled inside OMLoader (bioconda TU) — not here.
    # ImzMLFile.cpp is Xerces-free: it delegates all XML work to loadImzMLFull().
    if(HAS_BIOCONDA_OPENMS)
        add_library(openms_imzml STATIC
            openms_integration/src/ImzMLFile.cpp
            openms_integration/src/OnDiscImzMLExperiment.cpp
        )
    else()
        add_library(openms_imzml STATIC
            openms_integration/src/ImzMLFile.cpp
            openms_integration/src/OnDiscImzMLExperiment.cpp
            openms_integration/src/OMLoaderStub.cpp  # stub when bioconda absent
        )
    endif()

    target_include_directories(openms_imzml
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/openms_integration/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            # Native parser headers (imzml/ImzMLFile.h, ImzMLSpectrum.h, etc.)
            # Required by OnDiscImzMLExperiment.cpp (PIMPL impl); not exposed in the
            # public header, so it is safe to keep this PRIVATE.
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            # OMLoader.h bridge — separate from openms_integration/include/ so it
            # doesn't shadow bioconda OpenMS/* headers when openms_loader_obj compiles.
            ${CMAKE_CURRENT_SOURCE_DIR}/openms_integration/include_bridge
    )

    # Hide all symbols by default so dyld cannot replace our local weak-def
    # inline-function instantiations (MSExperiment::size, push_back, etc.) with
    # the incompatible strong definitions exported by bioconda libOpenMS.dylib.
    # Without this, dyld picks bioconda's MSExperiment::size (which uses a
    # different memory layout) and returns garbage / crashes.
    # PUBLIC: propagates to all targets that link openms_imzml (tests, imzml, server).
    target_compile_options(openms_imzml PUBLIC
        -fvisibility=hidden
        -fvisibility-inlines-hidden
    )

    # Real OpenMS Peak1D (OpenMS 3.5.0, bioconda) is embedded in our include
    # tree under openms_integration/include/OpenMS/KERNEL/Peak1D.h and its
    # transitive dependency chain (DPosition, Types, config, OpenMSConfig, …).
    # We embed these rather than adding the bioconda prefix to avoid pulling in
    # bioconda's Xerces 3.2 headers which conflict with Homebrew/apt Xerces 3.3.
    #
    # config.h → boost/current_function.hpp.
    # On macOS: use Homebrew Boost. On Linux: Boost is in the conda env.
    if(APPLE)
        execute_process(
            COMMAND brew --prefix boost
            OUTPUT_VARIABLE BREW_BOOST_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(BREW_BOOST_PREFIX)
            message(STATUS "  Boost (for real OpenMS Peak1D): ${BREW_BOOST_PREFIX}")
            target_include_directories(openms_imzml SYSTEM PUBLIC "${BREW_BOOST_PREFIX}/include")
        else()
            message(FATAL_ERROR "  Homebrew Boost not found — required by real OpenMS Peak1D headers")
        endif()
    else()
        # Linux: Boost headers come from the conda env or system
        if(EXISTS "${OPENMS_CONDA_ENV}/include/boost")
            message(STATUS "  Boost (conda): ${OPENMS_CONDA_ENV}/include/boost")
            target_include_directories(openms_imzml SYSTEM PUBLIC "${OPENMS_CONDA_ENV}/include")
        else()
            find_package(Boost QUIET)
            if(Boost_FOUND)
                message(STATUS "  Boost (system): ${Boost_INCLUDE_DIRS}")
                target_include_directories(openms_imzml SYSTEM PUBLIC "${Boost_INCLUDE_DIRS}")
            else()
                message(WARNING "  Boost not found — OpenMS Peak1D headers may fail to compile")
            endif()
        endif()
    endif()

    if(HAS_BIOCONDA_OPENMS)
        # Link the omloader SHARED library (ODR-safe: bioconda OpenMS symbols
        # stay bound to libOpenMS.dylib within omloader.dylib, never conflict
        # with our local OpenMS::MSSpectrum wrapper in libopenms_imzml.a)
        target_link_libraries(openms_imzml PUBLIC omloader)
        set_target_properties(openms_imzml PROPERTIES
            INSTALL_RPATH "${OPENMS_CONDA_ENV}/lib;${CMAKE_BINARY_DIR}/lib"
            BUILD_RPATH   "${OPENMS_CONDA_ENV}/lib;${CMAKE_BINARY_DIR}/lib"
        )
        message(STATUS "  OpenMS::MzMLFile (Phase 1 parser): ENABLED (ODR-safe dylib)")
    else()
        message(STATUS "  OpenMS::MzMLFile (Phase 1 parser): DISABLED (bioconda not found)")
    endif()

    # OnDiscImzMLExperiment.cpp calls imzml::ImzMLFile / IBDReader directly;
    # propagate imzml_parser so every consumer of openms_imzml resolves those symbols.
    target_link_libraries(openms_imzml PUBLIC imzml_parser)

    # --- CLI tool: imzml -------------------------------------------------------
    add_executable(imzml tools/imzml_cli.cpp)
    target_link_libraries(imzml PRIVATE openms_imzml)
    set_target_properties(imzml PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

    # --- Integration test -------------------------------------------------------
    if(BUILD_TESTS)
        add_executable(test_openms_imzml
            openms_integration/tests/test_openms_imzml.cpp
        )
        target_link_libraries(test_openms_imzml PRIVATE openms_imzml)
        set_target_properties(test_openms_imzml PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
        )

        add_test(
            NAME openms_imzml_integration
            COMMAND ${CMAKE_BINARY_DIR}/tests/test_openms_imzml
                    ${CMAKE_BINARY_DIR}/tests/data/Example_Continuous.imzML
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests
        )
        add_dependencies(test_openms_imzml generate_test_data)
    endif()

    # --- Web viewer: imzml_server -------------------------------------------
    add_executable(imzml_server tools/imzml_server.cpp)
    # Links both openms_imzml (for loading via OpenMS) AND imzml_parser
    # (for imzml::ImzMLWriter used by the /api/export endpoint).
    target_link_libraries(imzml_server PRIVATE openms_imzml imzml_parser)
    set_target_properties(imzml_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    if(HAS_BIOCONDA_OPENMS)
        set_target_properties(imzml_server PROPERTIES
            INSTALL_RPATH "${OPENMS_CONDA_ENV}/lib;${CMAKE_BINARY_DIR}/lib"
            BUILD_RPATH   "${OPENMS_CONDA_ENV}/lib;${CMAKE_BINARY_DIR}/lib"
        )
    endif()

    # --- Python bindings: imzml_ext (nanobind) --------------------------------
    option(BUILD_PYTHON_BINDINGS "Build nanobind Python extension imzml_ext" ON)

    if(BUILD_PYTHON_BINDINGS)
        # Locate the venv / active interpreter
        find_package(Python 3.8 REQUIRED COMPONENTS Interpreter Development.Module)

        # Ask the active Python where nanobind's CMake files live.
        # This honours the project .venv without requiring a system install.
        execute_process(
            COMMAND "${Python_EXECUTABLE}" -c
                    "import nanobind; print(nanobind.cmake_dir())"
            OUTPUT_VARIABLE NB_CMAKE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE NB_QUERY_RESULT
            ERROR_QUIET
        )

        if(NOT NB_QUERY_RESULT EQUAL 0 OR NOT NB_CMAKE_DIR)
            message(WARNING
                "nanobind not found in active Python env — "
                "run: pip install nanobind   (Python bindings skipped)")
        else()
            list(APPEND CMAKE_PREFIX_PATH "${NB_CMAKE_DIR}")
            find_package(nanobind CONFIG REQUIRED)

            nanobind_add_module(imzml_ext STABLE_ABI
                python/imzml_ext.cpp
            )

            # imzml_ext re-uses openms_imzml (static) + omloader (shared).
            # openms_imzml already propagates -fvisibility=hidden which
            # is correct for a Python extension (only NB_MODULE is visible).
            target_link_libraries(imzml_ext PRIVATE openms_imzml)

            # Embed RPATH so the .so finds libOpenMS.dylib / libomloader.dylib
            # from the conda env without an LD_LIBRARY_PATH tweak.
            if(HAS_BIOCONDA_OPENMS)
                set_target_properties(imzml_ext PROPERTIES
                    INSTALL_RPATH "${OPENMS_CONDA_ENV}/lib;${CMAKE_BINARY_DIR}/lib"
                    BUILD_RPATH   "${OPENMS_CONDA_ENV}/lib;${CMAKE_BINARY_DIR}/lib"
                )
            endif()

            # Drop the extension into build/python/ for easy import during dev:
            #   PYTHONPATH=build/release/python python3 -c "import imzml_ext"
            set_target_properties(imzml_ext PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python"
            )

            message(STATUS "  Python extension: ${CMAKE_BINARY_DIR}/python/imzml_ext<suffix>")

            # --- Python bindings: imzml_pyoms_ext (nanobind pyopenms bridge) --
            # imzml_pyoms_ext returns raw numpy arrays + dicts so that the pure
            # Python wrapper (python/imzml_pyoms.py) can build real
            # pyopenms.MSSpectrum / MSExperiment objects without any pybind11 /
            # nanobind ABI conflict.
            nanobind_add_module(imzml_pyoms_ext STABLE_ABI
                python/imzml_pyoms_ext.cpp
            )

            target_link_libraries(imzml_pyoms_ext PRIVATE openms_imzml)

            if(HAS_BIOCONDA_OPENMS)
                set_target_properties(imzml_pyoms_ext PROPERTIES
                    INSTALL_RPATH "${OPENMS_CONDA_ENV}/lib;${CMAKE_BINARY_DIR}/lib"
                    BUILD_RPATH   "${OPENMS_CONDA_ENV}/lib;${CMAKE_BINARY_DIR}/lib"
                )
            endif()

            set_target_properties(imzml_pyoms_ext PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python"
            )

            # Copy the pure-Python wrapper alongside the extension .so so that
            # "PYTHONPATH=build/release/python python3 -c 'import imzml_pyoms'"
            # works straight from the build tree without an install step.
            configure_file(
                "${CMAKE_SOURCE_DIR}/python/imzml_pyoms.py"
                "${CMAKE_BINARY_DIR}/python/imzml_pyoms.py"
                COPYONLY
            )

            message(STATUS "  Python extension: ${CMAKE_BINARY_DIR}/python/imzml_pyoms_ext<suffix>")
            message(STATUS "  Python wrapper:   ${CMAKE_BINARY_DIR}/python/imzml_pyoms.py")
        endif()
    endif()

    message(STATUS "  OpenMS integration: ENABLED (Xerces-C)")
    message(STATUS "  CLI tool:    ${CMAKE_BINARY_DIR}/bin/imzml")
    message(STATUS "  Web server:  ${CMAKE_BINARY_DIR}/bin/imzml_server")
else()
    message(STATUS "  OpenMS integration: DISABLED")
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS imzml_parser
    EXPORT  imzml_parserTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT imzml_parserTargets
    FILE   imzml_parserTargets.cmake
    NAMESPACE imzml::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/imzml_parser
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "  imzml_parser ${PROJECT_VERSION}")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler          : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build tests       : ${BUILD_TESTS}")
message(STATUS "  OpenMS integration: ${BUILD_OPENMS_INTEGRATION}")
if(CCACHE_PROGRAM)
message(STATUS "  ccache            : ${CCACHE_PROGRAM}")
endif()
message(STATUS "")

# ---------------------------------------------------------------------------
# CPack — produce distributable tarballs / installers
#   cmake --build build --target package   => imzml_parser-1.0.0-Darwin.tar.gz
#
# Artifacts produced:
#   include/imzml/         — standalone parser public headers
#   include/OpenMS/        — OpenMS integration headers
#   lib/libimzml_parser.a
#   lib/libopenms_imzml.a  (if BUILD_OPENMS_INTEGRATION)
#   bin/imzml              (if BUILD_OPENMS_INTEGRATION)
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME            "imzml-parser")
set(CPACK_PACKAGE_VERSION         ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "imzML C++ parser with OpenMS integration")
set(CPACK_PACKAGE_VENDOR          "imzml")
set(CPACK_RESOURCE_FILE_README    "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_GENERATOR               "TGZ;ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
# Only install library + headers, not tests/data
set(CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR}" "${PROJECT_NAME}" "ALL" "/")
include(CPack)
