/*
 * UART UHCI DMA Controller Implementation
 * 
 * 使用预分配缓冲区池的持续接收模式
 */

#include "uart_uhci.h"

#include <cstring>
#include <vector>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "esp_private/gdma.h"
#include "esp_private/gdma_link.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/esp_cache_private.h"
#include "hal/uhci_ll.h"
#include "hal/uart_ll.h"
#include "soc/soc_caps.h"

static const char* kTag = "UartUhci";

// Alignment helper macros
#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))
#define MAX_OF(a, b) (((a) > (b)) ? (a) : (b))

// C-style GDMA callback wrappers (must match gdma_event_callback_t signature)
static bool IRAM_ATTR gdma_rx_callback_wrapper(gdma_channel_handle_t dma_chan, gdma_event_data_t* event_data, void* user_data) {
    (void)dma_chan;
    auto* self = static_cast<UartUhci*>(user_data);
    return self->HandleGdmaRxDone(event_data->flags.normal_eof);
}

// Platform singleton for UHCI controller management
static struct {
    _lock_t mutex;
    void* controllers[SOC_UHCI_NUM];
} s_platform = {};

UartUhci::UartUhci() = default;

UartUhci::~UartUhci() {
    Deinit();
}

esp_err_t UartUhci::Init(const Config& config) {
    esp_err_t ret = ESP_OK;

    // Validate buffer pool config
    ESP_RETURN_ON_FALSE(config.rx_pool.buffer_count >= 2, ESP_ERR_INVALID_ARG, kTag, 
                        "buffer pool needs at least 2 buffers");
    ESP_RETURN_ON_FALSE(config.rx_pool.buffer_size > 0, ESP_ERR_INVALID_ARG, kTag, 
                        "buffer size must be > 0");

    uart_port_ = config.uart_port;

    // Find a free UHCI controller
    bool found = false;
    _lock_acquire(&s_platform.mutex);
    for (int i = 0; i < SOC_UHCI_NUM; i++) {
        if (s_platform.controllers[i] == nullptr) {
            s_platform.controllers[i] = this;
            uhci_num_ = i;
            found = true;
            break;
        }
    }
    _lock_release(&s_platform.mutex);
    ESP_RETURN_ON_FALSE(found, ESP_ERR_NOT_FOUND, kTag, "no free UHCI controller");

    // Enable UHCI bus clock
    PERIPH_RCC_ATOMIC() {
        uhci_ll_enable_bus_clock(uhci_num_, true);
        uhci_ll_reset_register(uhci_num_);
    }

    // Get UHCI hardware device
    uhci_dev_ = UHCI_LL_GET_HW(uhci_num_);
    ESP_GOTO_ON_FALSE(uhci_dev_, ESP_ERR_INVALID_STATE, err, kTag, "failed to get UHCI device");

    // Initialize UHCI hardware
    uhci_ll_init(uhci_dev_);
    uhci_ll_attach_uart_port(uhci_dev_, config.uart_port);

    // Disable separator character (otherwise UHCI may lose data)
    {
        uhci_seper_chr_t seper_chr = {};
        seper_chr.sub_chr_en = 0;
        uhci_ll_set_seper_chr(uhci_dev_, &seper_chr);
    }

    // Enable idle EOF mode - triggers callback when UART line becomes idle
    // 启用 idle EOF 模式 - 当 UART 线路空闲时触发回调
        uhci_ll_rx_set_eof_mode(uhci_dev_, UHCI_RX_IDLE_EOF);

    // Create PM lock
#if CONFIG_PM_ENABLE
    char pm_lock_name[16];
    snprintf(pm_lock_name, sizeof(pm_lock_name), "uhci_%d", uhci_num_);
    ESP_GOTO_ON_ERROR(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, pm_lock_name, &pm_lock_),
                      err, kTag, "failed to create PM lock");
#endif

    // Get cache line sizes
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &ext_mem_cache_line_);
    esp_cache_get_alignment(MALLOC_CAP_INTERNAL, &int_mem_cache_line_);

    // Initialize GDMA
    ESP_GOTO_ON_ERROR(InitGdma(config), err, kTag, "failed to initialize GDMA");

    // Initialize RX buffer pool
    ESP_GOTO_ON_ERROR(InitRxBufferPool(config.rx_pool), err, kTag, "failed to initialize RX buffer pool");

    ESP_LOGI(kTag, "UHCI %d initialized (UART %d), RX pool: %d x %d bytes", 
             uhci_num_, config.uart_port, config.rx_pool.buffer_count, config.rx_pool.buffer_size);
    return ESP_OK;

err:
    Deinit();
    return ret;
}

void UartUhci::Deinit() {
    // Stop any ongoing RX
    if (rx_running_.load()) {
        StopReceive();
    }

    // Deinitialize GDMA
    DeinitGdma();

    // Deinitialize RX buffer pool
    DeinitRxBufferPool();

    // Delete PM lock
    if (pm_lock_) {
        esp_pm_lock_delete(pm_lock_);
        pm_lock_ = nullptr;
    }

    // Disable UHCI clock
    if (uhci_num_ >= 0) {
        PERIPH_RCC_ATOMIC() {
            uhci_ll_enable_bus_clock(uhci_num_, false);
        }

        // Release controller slot
        _lock_acquire(&s_platform.mutex);
        s_platform.controllers[uhci_num_] = nullptr;
        _lock_release(&s_platform.mutex);
        uhci_num_ = -1;
    }

    uhci_dev_ = nullptr;
}

esp_err_t UartUhci::InitGdma(const Config& config) {
    // TX DMA is disabled to save GDMA channels on resources-constrained chips like ESP32-C5.
    // Standard UART FIFO writing will be used in Transmit().

    // Allocate RX DMA channel
    gdma_channel_alloc_config_t rx_alloc = {};
    rx_alloc.direction = GDMA_CHANNEL_DIRECTION_RX;
    ESP_RETURN_ON_ERROR(gdma_new_ahb_channel(&rx_alloc, &rx_dma_chan_), kTag, "RX DMA alloc failed");
    gdma_connect(rx_dma_chan_, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_UHCI, 0));

    gdma_transfer_config_t transfer_cfg = {};
    transfer_cfg.max_data_burst_size = config.dma_burst_size;
    transfer_cfg.access_ext_mem = true;
    ESP_RETURN_ON_ERROR(gdma_config_transfer(rx_dma_chan_, &transfer_cfg), kTag, "RX DMA config failed");

    // Get RX alignment constraints
    gdma_get_alignment_constraints(rx_dma_chan_, &rx_int_mem_align_, &rx_ext_mem_align_);

    // Create RX DMA link list with buffer pool size
    // Each buffer gets one DMA node
    gdma_link_list_config_t rx_link_cfg = {};
    rx_link_cfg.item_alignment = 4;
    rx_link_cfg.num_items = config.rx_pool.buffer_count;
    ESP_RETURN_ON_ERROR(gdma_new_link_list(&rx_link_cfg, &rx_dma_link_), kTag, "RX link list failed");

    // Register RX callback - use recv_done for each node completion
    gdma_rx_event_callbacks_t rx_cbs = {};
    rx_cbs.on_recv_done = gdma_rx_callback_wrapper;
    ESP_RETURN_ON_ERROR(gdma_register_rx_event_callbacks(rx_dma_chan_, &rx_cbs, this), kTag, "RX callback failed");

    return ESP_OK;
}

void UartUhci::DeinitGdma() {
    if (rx_dma_chan_) {
        gdma_disconnect(rx_dma_chan_);
        gdma_del_channel(rx_dma_chan_);
        rx_dma_chan_ = nullptr;
    }
    if (rx_dma_link_) {
        gdma_del_link_list(rx_dma_link_);
        rx_dma_link_ = nullptr;
    }
}

esp_err_t UartUhci::InitRxBufferPool(const BufferPoolConfig& config) {
    rx_pool_size_ = config.buffer_count;
    rx_buffer_size_ = config.buffer_size;

    // Calculate alignment requirements
    size_t max_align = MAX_OF(MAX_OF(rx_int_mem_align_, rx_ext_mem_align_), int_mem_cache_line_);
    if (max_align == 0) max_align = 4;

    // Align buffer size
    size_t aligned_size = ALIGN_UP(config.buffer_size, max_align);
    rx_buffer_size_ = aligned_size;
    rx_cache_line_ = int_mem_cache_line_;

    // Allocate buffer descriptor array
    rx_buffer_pool_ = static_cast<RxBuffer*>(heap_caps_calloc(rx_pool_size_, sizeof(RxBuffer), MALLOC_CAP_INTERNAL));
    ESP_RETURN_ON_FALSE(rx_buffer_pool_, ESP_ERR_NO_MEM, kTag, "failed to allocate buffer pool descriptors");

    // Create free buffer queue
    rx_free_queue_ = xQueueCreateWithCaps(rx_pool_size_, sizeof(uint32_t), MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(rx_free_queue_, ESP_ERR_NO_MEM, kTag, "failed to create free buffer queue");

    // Allocate mounted buffer index tracking array
    rx_mounted_idx_ = static_cast<int32_t*>(heap_caps_calloc(rx_pool_size_, sizeof(int32_t), MALLOC_CAP_INTERNAL));
    ESP_RETURN_ON_FALSE(rx_mounted_idx_, ESP_ERR_NO_MEM, kTag, "failed to allocate mounted index array");
    for (size_t i = 0; i < rx_pool_size_; i++) {
        rx_mounted_idx_[i] = -1;  // No buffer mounted
    }
    rx_mounted_count_ = 0;

    // Allocate individual buffers and add to free queue
    for (size_t i = 0; i < rx_pool_size_; i++) {
        rx_buffer_pool_[i].data = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(max_align, aligned_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        ESP_RETURN_ON_FALSE(rx_buffer_pool_[i].data, ESP_ERR_NO_MEM, kTag, 
                            "failed to allocate buffer %d", i);
        
        rx_buffer_pool_[i].capacity = aligned_size;
        rx_buffer_pool_[i].size = 0;
        rx_buffer_pool_[i].index = i;

        // Add to free queue
        uint32_t idx = i;
        xQueueSend(rx_free_queue_, &idx, 0);
    }

    ESP_LOGD(kTag, "RX buffer pool: %d buffers x %d bytes (aligned to %d)", 
             rx_pool_size_, aligned_size, max_align);

    return ESP_OK;
}

void UartUhci::DeinitRxBufferPool() {
    if (rx_buffer_pool_) {
        for (size_t i = 0; i < rx_pool_size_; i++) {
            if (rx_buffer_pool_[i].data) {
                heap_caps_free(rx_buffer_pool_[i].data);
            }
        }
        heap_caps_free(rx_buffer_pool_);
        rx_buffer_pool_ = nullptr;
    }

    if (rx_mounted_idx_) {
        heap_caps_free(rx_mounted_idx_);
        rx_mounted_idx_ = nullptr;
    }

    if (rx_free_queue_) {
        vQueueDeleteWithCaps(rx_free_queue_);
        rx_free_queue_ = nullptr;
    }

    rx_pool_size_ = 0;
    rx_buffer_size_ = 0;
    rx_mounted_count_ = 0;
}

void UartUhci::SetRxCallback(RxCallback callback, void* user_data) {
    rx_callback_ = callback;
    rx_callback_user_data_ = user_data;
}

esp_err_t UartUhci::StartReceive() {
    ESP_RETURN_ON_FALSE(!rx_running_.load(), ESP_ERR_INVALID_STATE, kTag, "RX already running");
    ESP_RETURN_ON_FALSE(rx_buffer_pool_, ESP_ERR_INVALID_STATE, kTag, "buffer pool not initialized");

    // Only mount half of the buffers, keep the rest in free queue for replacement
    // This ensures we always have buffers available when receiving data
    size_t max_mount = (rx_pool_size_ + 1) / 2;  // Mount at most half (rounded up)
    if (max_mount < 2) max_mount = 2;
    
    size_t mounted = 0;
    std::vector<gdma_buffer_mount_config_t> mount_configs(rx_pool_size_);
    memset(mount_configs.data(), 0, mount_configs.size() * sizeof(gdma_buffer_mount_config_t));

    // Clear mounted tracking
    for (size_t i = 0; i < rx_pool_size_; i++) {
        rx_mounted_idx_[i] = -1;
    }

    for (size_t i = 0; i < max_mount; i++) {
        uint32_t idx;
        if (xQueueReceive(rx_free_queue_, &idx, 0) != pdTRUE) {
            break;  // No more free buffers
        }

        RxBuffer* buf = &rx_buffer_pool_[idx];
        buf->size = 0;

        mount_configs[mounted].buffer = buf->data;
        mount_configs[mounted].length = buf->capacity;
        
        // Track which buffer is mounted at this DMA node
        rx_mounted_idx_[mounted] = idx;
        mounted++;
    }

    ESP_RETURN_ON_FALSE(mounted >= 2, ESP_ERR_INVALID_STATE, kTag, 
                        "need at least 2 free buffers to start");

    rx_mounted_count_ = mounted;
    
    ESP_LOGD(kTag, "StartReceive: mounted %d buffers, %d in free queue", 
             mounted, uxQueueMessagesWaiting(rx_free_queue_));

    // Acquire PM lock
    if (pm_lock_) {
        esp_pm_lock_acquire(pm_lock_);
    }

    rx_active_node_ = 0;
    rx_running_.store(true);

    // Mount buffers and start circular DMA
    gdma_link_mount_buffers(rx_dma_link_, 0, mount_configs.data(), mounted, nullptr);
    gdma_reset(rx_dma_chan_);
    gdma_start(rx_dma_chan_, gdma_link_get_head_addr(rx_dma_link_));

    ESP_LOGD(kTag, "RX started with %d buffers", mounted);
    return ESP_OK;
}

esp_err_t UartUhci::StopReceive() {
    if (!rx_running_.load()) {
        return ESP_OK;  // Already stopped
    }

    // Stop and reset DMA
    gdma_stop(rx_dma_chan_);
    gdma_reset(rx_dma_chan_);

    rx_running_.store(false);

    // Release PM lock
    if (pm_lock_) {
        esp_pm_lock_release(pm_lock_);
    }

    // Return all mounted buffers back to free queue
    for (size_t i = 0; i < rx_pool_size_; i++) {
        if (rx_mounted_idx_[i] >= 0) {
            uint32_t idx = rx_mounted_idx_[i];
            xQueueSend(rx_free_queue_, &idx, 0);
            rx_mounted_idx_[i] = -1;
        }
    }
    rx_mounted_count_ = 0;

    ESP_LOGD(kTag, "RX stopped");
    return ESP_OK;
}

void UartUhci::ReturnBuffer(RxBuffer* buffer) {
    if (!buffer || buffer->index >= rx_pool_size_) {
        return;
    }

    buffer->size = 0;
    uint32_t idx = buffer->index;
    
    // Return to free queue
    BaseType_t ret = xQueueSend(rx_free_queue_, &idx, 0);
    if (ret != pdTRUE) {
        ESP_LOGW(kTag, "Failed to return buffer %lu to free queue", idx);
    }

    // If RX is running and we have a free buffer, try to add it to DMA
    // This is handled in the ISR when needed
}

esp_err_t UartUhci::Transmit(const uint8_t* buffer, size_t size) {
    ESP_RETURN_ON_FALSE(buffer && size > 0, ESP_ERR_INVALID_ARG, kTag, "invalid arguments");

    // Acquire PM lock for TX
    if (pm_lock_) {
        esp_pm_lock_acquire(pm_lock_);
    }

    // Standard UART FIFO writing (Synchronous)
    uart_dev_t *hw = UART_LL_GET_HW(uart_port_);
    const uint8_t* data = buffer;
    size_t remaining = size;

    while (remaining > 0) {
        uint32_t can_write = uart_ll_get_txfifo_len(hw);
        uint32_t to_write = (remaining < can_write) ? (uint32_t)remaining : can_write;
        if (to_write > 0) {
            uart_ll_write_txfifo(hw, data, to_write);
            data += to_write;
            remaining -= to_write;
        } else {
            esp_rom_delay_us(10); 
        }
    }

    // Wait for the last bytes to actually be sent out
    while (!uart_ll_is_tx_idle(hw)) {
        esp_rom_delay_us(10);
    }

    // Release PM lock
    if (pm_lock_) {
        esp_pm_lock_release(pm_lock_);
    }

    return ESP_OK;
}

bool UartUhci::HandleGdmaRxDone(bool is_eof) {
    bool need_yield = false;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (!rx_running_.load()) {
        return false;
    }

    // Get the DMA node that just completed
    size_t node_idx = rx_active_node_;
    
    // Get the buffer index mounted at this node
    int32_t buf_idx = rx_mounted_idx_[node_idx];
    if (buf_idx < 0) {
        // No buffer mounted at this node, restart DMA if EOF
        if (is_eof) {
            rx_active_node_ = 0;
            gdma_reset(rx_dma_chan_);
            gdma_start(rx_dma_chan_, gdma_link_get_head_addr(rx_dma_link_));
        }
        return false;
    }

    RxBuffer* buf = &rx_buffer_pool_[buf_idx];

    // Get received size - count from this node to EOF marker
    size_t rx_size = gdma_link_count_buffer_size_till_eof(rx_dma_link_, node_idx);
    
    // Sanity check: size should not exceed buffer capacity and should be > 0
    if (rx_size == 0 || rx_size > buf->capacity) {
        // No valid data or invalid size, skip processing but still manage buffer
        rx_mounted_idx_[node_idx] = -1;
        uint32_t idx = buf->index;
        xQueueSendFromISR(rx_free_queue_, &idx, &xHigherPriorityTaskWoken);
        
        if (is_eof) {
            rx_active_node_ = 0;
            gdma_reset(rx_dma_chan_);
            gdma_start(rx_dma_chan_, gdma_link_get_head_addr(rx_dma_link_));
        }
        return xHigherPriorityTaskWoken == pdTRUE;
    }
    buf->size = rx_size;

        // Sync cache if needed
    if (rx_cache_line_ > 0 && rx_size > 0) {
        size_t sync_size = ALIGN_UP(rx_size, rx_cache_line_);
        esp_cache_msync(buf->data, sync_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    }

    // Mark this node as no longer having a buffer (will be replaced or left empty)
    rx_mounted_idx_[node_idx] = -1;

    // Move to next node (circular within mounted count)
    rx_active_node_ = (rx_active_node_ + 1) % rx_mounted_count_;

    // Try to get a free buffer to replace the one we're about to deliver
    uint32_t free_idx;
    if (xQueueReceiveFromISR(rx_free_queue_, &free_idx, &xHigherPriorityTaskWoken) == pdTRUE) {
        // Mount the new buffer at the completed node position
        RxBuffer* new_buf = &rx_buffer_pool_[free_idx];
        new_buf->size = 0;

        gdma_buffer_mount_config_t mount = {};
        mount.buffer = new_buf->data;
        mount.length = new_buf->capacity;
        
        // Mount at the position of the completed buffer
        gdma_link_mount_buffers(rx_dma_link_, node_idx, &mount, 1, nullptr);
        
        // Track the new buffer at this node
        rx_mounted_idx_[node_idx] = free_idx;

        if (xHigherPriorityTaskWoken) {
            need_yield = true;
        }
    } else {
        // No free buffer available - this is a problem!
        // The DMA node will have no buffer until one is returned
        ESP_DRAM_LOGE(kTag, "No free buffer for DMA node %d!", node_idx);
    }

    // User callback to deliver the completed buffer
    if (rx_callback_ && rx_size > 0) {
            RxEventData data = {
            .buffer = buf,
                .recv_size = rx_size,
            };
            if (rx_callback_(data, rx_callback_user_data_)) {
                need_yield = true;
            }
    } else if (rx_size == 0) {
        // Empty buffer, return it immediately
        uint32_t idx = buf->index;
        xQueueSendFromISR(rx_free_queue_, &idx, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            need_yield = true;
        }
    }

    // If EOF (idle detected), restart DMA from beginning to continue receiving
    if (is_eof) {
        rx_active_node_ = 0;
        gdma_reset(rx_dma_chan_);
        gdma_start(rx_dma_chan_, gdma_link_get_head_addr(rx_dma_link_));
    }

    return need_yield;
}
