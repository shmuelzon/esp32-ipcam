file(GLOB srcs "opus/src/*.c")
file(GLOB silk "opus/silk/*.c")
file(GLOB celt "opus/celt/*.c")
file(GLOB fixed "opus/silk/fixed/*.c")
#file(GLOB float "opus/silk/float/*.c")

idf_component_register(SRCS "${srcs}" "${silk}" "${celt}" "${fixed}" "${float}"
                       INCLUDE_DIRS .
					                "opus/include"
									"opus/silk"
									"opus/silk/fixed"
									"opus/silk/float"
									"opus/celt"
									)

component_compile_options(
    -Wno-stringop-overread
    -Wno-stringop-overflow
    -Wno-maybe-uninitialized)

target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-DHAVE_CONFIG_H")
