# Stage the pinned Cubiomes sources used by the GPU translation unit. The
# linked submodule stays pristine; NVIDIA-specific declarations live only in
# this build-tree copy.
if(NOT DEFINED CUBIOMES_SOURCE_DIR OR NOT DEFINED CUBIOMES_DEVICE_OVERLAY_DIR)
    message(FATAL_ERROR "Set CUBIOMES_SOURCE_DIR and CUBIOMES_DEVICE_OVERLAY_DIR before including this file.")
endif()

if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE CUBIOMES_DEVICE_INPUTS
        "${CUBIOMES_SOURCE_DIR}/*.c"
        "${CUBIOMES_SOURCE_DIR}/*.h")
else()
    file(GLOB_RECURSE CUBIOMES_DEVICE_INPUTS CONFIGURE_DEPENDS
        "${CUBIOMES_SOURCE_DIR}/*.c"
        "${CUBIOMES_SOURCE_DIR}/*.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CUBIOMES_DEVICE_INPUTS})
endif()

file(COPY "${CUBIOMES_SOURCE_DIR}/"
    DESTINATION "${CUBIOMES_DEVICE_OVERLAY_DIR}/cubiomes"
    PATTERN ".git" EXCLUDE)

if(CVHIP_NVIDIA_CLANG_CUDA)
    function(cvhip_patch_nvidia_table relative_path symbol dimensions initializer)
        set(_path "${CUBIOMES_DEVICE_OVERLAY_DIR}/cubiomes/${relative_path}")
        file(READ "${_path}" _contents)
        set(_declaration "static const uint64_t ${symbol}${dimensions} =${initializer}")
        string(FIND "${_contents}" "${_declaration}" _declaration_position)
        if(_declaration_position LESS 0)
            message(FATAL_ERROR
                "Expected the upstream declaration for ${symbol} in ${relative_path}; "
                "the pinned Cubiomes source may have changed.")
        endif()
        string(LENGTH "${_declaration}" _declaration_length)
        math(EXPR _search_start "${_declaration_position} + ${_declaration_length}")
        string(SUBSTRING "${_contents}" ${_search_start} -1 _tail)
        string(FIND "${_tail}" "${_declaration}" _duplicate_position)
        if(NOT _duplicate_position LESS 0)
            message(FATAL_ERROR "Found duplicate ${symbol} declaration in ${relative_path}.")
        endif()
        set(_device_declaration
            "#if defined(__HIP_PLATFORM_NVIDIA__) && defined(__CUDA_ARCH__)\nstatic __device__ uint64_t ${symbol}${dimensions} =${initializer}\n#else\nstatic const uint64_t ${symbol}${dimensions} =${initializer}\n#endif")
        string(REPLACE "${_declaration}" "${_device_declaration}" _contents "${_contents}")
        file(WRITE "${_path}" "${_contents}")
    endfunction()

    cvhip_patch_nvidia_table("xrms.h" "xrms" "[POWS_OF_2][128][2]" " {")
    cvhip_patch_nvidia_table("tables/btree18.h" "btree18_nodes" "[]" "")
    cvhip_patch_nvidia_table("tables/btree19.h" "btree19_nodes" "[]" "")
    cvhip_patch_nvidia_table("tables/btree192.h" "btree192_nodes" "[]" "")
    cvhip_patch_nvidia_table("tables/btree20.h" "btree20_nodes" "[]" "")
    cvhip_patch_nvidia_table("tables/btree21wd.h" "btree21wd_nodes" "[]" "")
    cvhip_patch_nvidia_table("tables/btree215.h" "btree215_nodes" "[]" "")
    cvhip_patch_nvidia_table("tables/btree262.h" "btree262_nodes" "[]" "")
    cvhip_patch_nvidia_table("tables/btree263.h" "btree263_nodes" "[]" "")
endif()
