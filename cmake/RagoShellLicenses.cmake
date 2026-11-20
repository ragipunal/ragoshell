# cmake/RagoShellLicenses.cmake
#
# Modular installation rules for the open-source & third-party license bundle
# (Roadmap Phase 3.7 / A5, docs/ReleasePipeline.md).
#
# Installs under COMPONENT "licenses" (included in default `cmake --install` ALL):
#   - macOS:          <prefix>/<RAGO_PRODUCT_NAME>.app/Contents/Resources/licenses/
#   - Windows/Linux:  <prefix>/licenses/
#     (overridable via -DRAGO_LICENSES_INSTALL_DIR=<relative-path>)
#
# Contents:
#   1. Root notices: LICENSE, LICENSE.LGPLv3, COPYRIGHT, THIRD-PARTY-NOTICES.txt
#   2. Embedded third-party & font notices under third-party/<component>-*:
#      qscintilla, qjson, googletest, DejaVuSans
#   3. Installed vcpkg port copyright notices (when present under
#      vcpkg_installed/<triplet>/share/<port>/copyright) under collision-free
#      names: third-party/<port>-COPYRIGHT

if(NOT RAGO_PRODUCT_NAME)
    message(FATAL_ERROR "RagoShellLicenses.cmake requires RagoShellDefaults.cmake (RAGO_PRODUCT_NAME is empty).")
endif()

if(SYSTEM_MACOSX)
    set(_rago_default_licenses_dir "${RAGO_PRODUCT_NAME}.app/Contents/Resources/licenses")
else()
    set(_rago_default_licenses_dir "licenses")
endif()

set(RAGO_LICENSES_INSTALL_DIR "${_rago_default_licenses_dir}"
    CACHE STRING "Relative install destination for bundled open-source license notices")

function(_rago_require_nonempty_license_file filepath label)
    if(NOT EXISTS "${filepath}" OR IS_DIRECTORY "${filepath}" OR IS_SYMLINK "${filepath}")
        message(FATAL_ERROR "Required license notice (${label}) is missing or not a regular file: ${filepath}")
    endif()
    file(SIZE "${filepath}" _rago_lic_size)
    if(_rago_lic_size LESS 1)
        message(FATAL_ERROR "Required license notice (${label}) is empty (0 bytes): ${filepath}")
    endif()
endfunction()

set(_rago_root_license_files
    "${CMAKE_SOURCE_DIR}/LICENSE"
    "${CMAKE_SOURCE_DIR}/LICENSE.LGPLv3"
    "${CMAKE_SOURCE_DIR}/COPYRIGHT"
    "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt"
)

foreach(_rago_root_lic IN LISTS _rago_root_license_files)
    _rago_require_nonempty_license_file("${_rago_root_lic}" "root notice")
endforeach()

install(FILES ${_rago_root_license_files}
    DESTINATION "${RAGO_LICENSES_INSTALL_DIR}"
    COMPONENT licenses)

# Embedded third-party & bundled font license files under collision-free names.
set(_rago_embedded_license_specs
    "${CMAKE_SOURCE_DIR}/${QSCINTILLA_DIR}/sources/LICENSE.GPL3|qscintilla-LICENSE.GPL3"
    "${CMAKE_SOURCE_DIR}/${QSCINTILLA_DIR}/sources/OPENSOURCE-NOTICE.TXT|qscintilla-OPENSOURCE-NOTICE.txt"
    "${CMAKE_SOURCE_DIR}/${QJSON_DIR}/sources/README.license|qjson-LICENSE.txt"
    "${CMAKE_SOURCE_DIR}/${GOOGLE_TEST_DIR}/LICENSE|googletest-LICENSE.txt"
    "${CMAKE_SOURCE_DIR}/assets/fonts/DEJAVU-LICENSE|DejaVuSans-LICENSE.txt"
)

foreach(_rago_spec IN LISTS _rago_embedded_license_specs)
    string(REPLACE "|" ";" _rago_pair "${_rago_spec}")
    list(GET _rago_pair 0 _rago_src_file)
    list(GET _rago_pair 1 _rago_dst_name)
    _rago_require_nonempty_license_file("${_rago_src_file}" "embedded third-party notice ${_rago_dst_name}")
    install(FILES "${_rago_src_file}"
        DESTINATION "${RAGO_LICENSES_INSTALL_DIR}/third-party"
        RENAME "${_rago_dst_name}"
        COMPONENT licenses)
endforeach()

# Automatically collect installed vcpkg port copyright files at `cmake --install` time:
#   <vcpkg_installed>/<triplet>/share/<port>/copyright -> <dest>/third-party/<port>-COPYRIGHT
if(DEFINED VCPKG_INSTALLED_DIR AND NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "")
    set(_rago_vcpkg_installed_root "${VCPKG_INSTALLED_DIR}")
else()
    set(_rago_vcpkg_installed_root "${CMAKE_BINARY_DIR}/vcpkg_installed")
endif()
set(_rago_vcpkg_target_triplet "${VCPKG_TARGET_TRIPLET}")

install(CODE "
    set(_rago_vcpkg_base \"${_rago_vcpkg_installed_root}\")
    set(_rago_triplet \"${_rago_vcpkg_target_triplet}\")
    set(_rago_tp_dest \"\${CMAKE_INSTALL_PREFIX}/${RAGO_LICENSES_INSTALL_DIR}/third-party\")
    set(_rago_copyright_candidates \"\")

    if(EXISTS \"\${_rago_vcpkg_base}\" AND IS_DIRECTORY \"\${_rago_vcpkg_base}\" AND NOT IS_SYMLINK \"\${_rago_vcpkg_base}\")
        if(NOT \"\${_rago_triplet}\" STREQUAL \"\" AND IS_DIRECTORY \"\${_rago_vcpkg_base}/\${_rago_triplet}/share\")
            file(GLOB _rago_copyright_candidates LIST_DIRECTORIES false
                \"\${_rago_vcpkg_base}/\${_rago_triplet}/share/*/copyright\")
        else()
            file(GLOB _rago_copyright_candidates LIST_DIRECTORIES false
                \"\${_rago_vcpkg_base}/*/share/*/copyright\")
        endif()
    endif()

    if(_rago_copyright_candidates)
        list(SORT _rago_copyright_candidates)
        set(_rago_installed_ports \"\")
        foreach(_rago_cp_file IN LISTS _rago_copyright_candidates)
            if(IS_SYMLINK \"\${_rago_cp_file}\" OR IS_DIRECTORY \"\${_rago_cp_file}\")
                continue()
            endif()
            file(SIZE \"\${_rago_cp_file}\" _rago_cp_sz)
            if(_rago_cp_sz LESS 1)
                continue()
            endif()
            get_filename_component(_rago_port_dir \"\${_rago_cp_file}\" DIRECTORY)
            get_filename_component(_rago_port_name \"\${_rago_port_dir}\" NAME)
            if(NOT \"\${_rago_port_name}\" MATCHES \"^[A-Za-z0-9][A-Za-z0-9._+-]*\$\")
                continue()
            endif()
            list(FIND _rago_installed_ports \"\${_rago_port_name}\" _rago_already_seen)
            if(NOT _rago_already_seen EQUAL -1)
                continue()
            endif()
            list(APPEND _rago_installed_ports \"\${_rago_port_name}\")
            file(INSTALL
                DESTINATION \"\${_rago_tp_dest}\"
                TYPE FILE
                RENAME \"\${_rago_port_name}-COPYRIGHT\"
                FILES \"\${_rago_cp_file}\")
        endforeach()
    endif()
" COMPONENT licenses)
