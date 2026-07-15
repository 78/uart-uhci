if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(UART_UHCI_DMA_COMPONENT esp_driver_dma)
else()
    set(UART_UHCI_DMA_COMPONENT esp_hw_support)
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
        ${UART_UHCI_DMA_COMPONENT}
)
