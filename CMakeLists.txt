if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(UART_UHCI_DMA_COMPONENT esp_driver_dma)
else()
    set(UART_UHCI_DMA_COMPONENT esp_hw_support)
endif()

# ESP-IDF master renamed uhci_ll_rx_set_eof_mode() to
# uhci_ll_rx_enable_eof_modes() and added an enable argument. Both APIs are
# static inline functions, so neither the preprocessor nor the link stage can
# probe for them. Detect the declaration in the selected target's LL header
# while configuring the component instead of guessing from a development SDK
# version number.
idf_build_get_property(idf_path IDF_PATH)
idf_build_get_property(idf_target IDF_TARGET)
set(UART_UHCI_LL_HEADER)
foreach(uhci_hal_component esp_hal_uart hal)
    set(uhci_ll_header "${idf_path}/components/${uhci_hal_component}/${idf_target}/include/hal/uhci_ll.h")
    if(EXISTS "${uhci_ll_header}")
        set(UART_UHCI_LL_HEADER "${uhci_ll_header}")
        break()
    endif()
endforeach()

if(NOT UART_UHCI_LL_HEADER)
    message(FATAL_ERROR "uart-uhci: unable to locate hal/uhci_ll.h for ${idf_target}")
endif()

file(READ "${UART_UHCI_LL_HEADER}" uart_uhci_ll_contents)
if(uart_uhci_ll_contents MATCHES "uhci_ll_rx_enable_eof_modes[ \\t\\r\\n]*\\(")
    set(UART_UHCI_LL_HAS_ENABLE_EOF_MODES 1)
endif()

# ESP-IDF 6.2 replaced gdma_get_alignment_constraints() with
# gdma_get_channel_alignment_constraints() and changed the returned alignment
# data to a structure. Detect the declaration so this component also remains
# compatible with ESP-IDF 5.5, 6.0, and 6.1.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(UART_UHCI_GDMA_HEADER "${idf_path}/components/esp_driver_dma/include/esp_private/gdma.h")
else()
    set(UART_UHCI_GDMA_HEADER "${idf_path}/components/esp_hw_support/dma/include/esp_private/gdma.h")
endif()

if(NOT EXISTS "${UART_UHCI_GDMA_HEADER}")
    message(FATAL_ERROR "uart-uhci: unable to locate esp_private/gdma.h")
endif()

file(READ "${UART_UHCI_GDMA_HEADER}" uart_uhci_gdma_contents)
if(uart_uhci_gdma_contents MATCHES "gdma_get_channel_alignment_constraints[ \\t\\r\\n]*\\(")
    set(UART_UHCI_GDMA_HAS_CHANNEL_ALIGNMENT_INFO 1)
endif()

idf_component_register(
    SRCS
        "src/uart_uhci.cc"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_pm
        esp_mm
        esp_driver_uart
    PRIV_REQUIRES
        esp_timer
        ${UART_UHCI_DMA_COMPONENT}
)

if(UART_UHCI_LL_HAS_ENABLE_EOF_MODES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE UART_UHCI_LL_HAS_ENABLE_EOF_MODES=1)
endif()

if(UART_UHCI_GDMA_HAS_CHANNEL_ALIGNMENT_INFO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE UART_UHCI_GDMA_HAS_CHANNEL_ALIGNMENT_INFO=1)
endif()
