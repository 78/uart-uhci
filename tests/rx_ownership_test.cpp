#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <atomic>

#define IRAM_ATTR
#define portMUX_TYPE std::recursive_mutex
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL_SAFE(mux) (mux)->lock()
#define portEXIT_CRITICAL_SAFE(mux) (mux)->unlock()
using esp_err_t = int;
using esp_pm_lock_handle_t = void*;
using uart_port_t = int;
constexpr int UART_NUM_MAX = 3, ESP_OK = 0, ESP_ERR_INVALID_STATE = 1,
    ESP_ERR_INVALID_ARG = 2, ESP_ERR_TIMEOUT = 3;
#define ESP_RETURN_ON_FALSE(a, err, ...) do { if (!(a)) return err; } while (0)
#define ESP_DRAM_LOGW(...)
#define ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))
constexpr int ESP_CACHE_MSYNC_FLAG_DIR_M2C = 1;
enum gdma_lli_owner_t { GDMA_LLI_OWNER_CPU, GDMA_LLI_OWNER_DMA };
struct Descriptor { gdma_lli_owner_t owner = GDMA_LLI_OWNER_DMA; size_t length = 0; };
struct gdma_link_list_t { std::array<Descriptor, 4> desc; };
struct gdma_channel_t { unsigned restarts = 0; bool running = false; };
struct gdma_buffer_mount_config_t { void* buffer; size_t length; struct { int mark_eof; } flags; };
struct uart_dev_t {
    size_t available = 128, written = 0;
    bool idle = true;
    unsigned resets = 0;
};
uart_dev_t uart;
int pm_balance = 0;
int64_t now_us = 0;
std::atomic<bool>* cancel_on_delay = nullptr;
int64_t esp_timer_get_time() { return now_us; }
void esp_rom_delay_us(int us) {
    now_us += us;
    if (cancel_on_delay) *cancel_on_delay = true;
}
bool uart_ll_is_tx_idle(uart_dev_t* hw) { return hw->idle; }
size_t uart_ll_get_txfifo_len(uart_dev_t* hw) { return hw->available; }
void uart_ll_write_txfifo(uart_dev_t* hw, const uint8_t*, size_t count) { hw->written += count; }
void uart_ll_txfifo_rst(uart_dev_t* hw) { ++hw->resets; }
#define UART_LL_GET_HW(port) (&uart)
void uart_ll_rxfifo_rst(uart_dev_t*) {}
void esp_pm_lock_acquire(void*) { ++pm_balance; }
void esp_pm_lock_release(void*) { --pm_balance; }
void esp_cache_msync(void*, size_t, int) {}
int gdma_link_get_owner(gdma_link_list_t* l, int i, gdma_lli_owner_t* o) { *o = l->desc[i].owner; return 0; }
int gdma_link_set_owner(gdma_link_list_t* l, int i, gdma_lli_owner_t o) { l->desc[i].owner = o; return 0; }
size_t gdma_link_get_length(gdma_link_list_t* l, int i) { return l->desc[i].length; }
void gdma_link_mount_buffers(gdma_link_list_t* l, int i, gdma_buffer_mount_config_t* m, int, void*) {
    l->desc[i] = {GDMA_LLI_OWNER_DMA, m->length};
}
void gdma_reset(gdma_channel_t*) {}
void gdma_start(gdma_channel_t* c, uintptr_t) { c->running = true; ++c->restarts; }
void gdma_stop(gdma_channel_t* c) { c->running = false; }
void gdma_append(gdma_channel_t*) {}
// @HEADER@
UartUhci::UartUhci() = default;
UartUhci::~UartUhci() = default;
// @METHODS@

struct Fixture {
    UartUhci controller;
    gdma_link_list_t link;
    gdma_channel_t channel;
    std::array<UartUhci::RxBuffer, 4> buffers{};
    std::array<std::array<uint8_t, 64>, 4> data{};
    std::array<unsigned, 4> calls{};
    bool immediate = false, defer = false;
    Fixture() {
        controller.rx_pool_size_ = 4;
        controller.rx_buffer_pool_ = buffers.data();
        controller.rx_dma_link_ = &link;
        controller.rx_dma_chan_ = &channel;
        controller.rx_callback_user_data_ = this;
        controller.rx_callback_ = [](const UartUhci::RxEventData& event, void* arg) {
            auto& f = *static_cast<Fixture*>(arg);
            ++f.calls[event.buffer->index];
            if (f.immediate) f.controller.ReturnBuffer(event.buffer);
            if (f.defer) f.controller.DeferReturnBuffer(event.buffer);
            return false;
        };
        for (unsigned i = 0; i < 4; ++i) buffers[i] = {data[i].data(), 64, 0, i, false, false};
        assert(controller.StartReceive() == ESP_OK);
    }
    void complete(unsigned i, size_t size = 16) { link.desc[i] = {GDMA_LLI_OWNER_CPU, size}; }
};
int main() {
    {
        UartUhci tx;
        tx.pm_lock_ = &uart;
        const uint8_t data[256] = {};
        auto send = [&](unsigned ms, const std::atomic<bool>* cancel = nullptr) {
            now_us = 0;
            const auto balance = pm_balance;
            const auto result = tx.Transmit(data, sizeof(data), ms, cancel);
            assert(pm_balance == balance);
            return result;
        };
        assert(send(10) == ESP_OK && uart.written == 256 && uart.resets == 0);
        uart = {.available = 0};
        assert(send(10) == ESP_ERR_TIMEOUT && now_us == 10000 && uart.resets == 1);
        uart = {.idle = false};
        assert(send(10) == ESP_ERR_TIMEOUT && uart.written == 256 && uart.resets == 1);
        uart = {};
        assert(send(0) == ESP_ERR_TIMEOUT && uart.written == 0);
        std::atomic<bool> cancel{true};
        uart = {};
        assert(send(10, &cancel) == ESP_ERR_INVALID_STATE && uart.written == 0);
        cancel = false;
        uart = {.available = 0};
        cancel_on_delay = &cancel;
        assert(send(10, &cancel) == ESP_ERR_INVALID_STATE && now_us == 10 && uart.resets == 1);
        cancel_on_delay = nullptr;
        assert(tx.Transmit(nullptr, 1) == ESP_ERR_INVALID_ARG);
        assert(tx.Transmit(data, 0) == ESP_ERR_INVALID_ARG);
        puts("8 FIFO deadline/cancellation scenarios passed");
    }
    {
        Fixture f;
        for (unsigned i = 0; i < 4; ++i) { f.complete(i); f.controller.HandleGdmaRxDone(0, true); }
        f.controller.HandleGdmaRxDone(0, true);
        assert((f.calls == std::array<unsigned, 4>{1, 1, 1, 1}));
        for (auto& b : f.buffers) f.controller.ReturnBuffer(&b);
        // Duplicate return must not overwrite a fresh DMA completion.
        f.complete(0);
        f.controller.ReturnBuffer(&f.buffers[0]);
        assert(f.link.desc[0].owner == GDMA_LLI_OWNER_CPU);
        f.controller.HandleGdmaRxDone(0, true);
        assert(f.calls[0] == 2);
    }
    {
        Fixture f;
        f.complete(0); f.controller.HandleGdmaRxDone(0, true);
        f.controller.StopReceive();
        assert(f.controller.StartReceive() == ESP_ERR_INVALID_STATE);
        assert(f.buffers[0].size == 16);
        f.controller.ReturnBuffer(&f.buffers[0]);
        assert(f.controller.StartReceive() == ESP_OK);
    }
    {
        Fixture f;
        f.defer = true;
        for (unsigned i = 0; i < 4; ++i) f.complete(i);
        f.controller.HandleGdmaDescrErr();
        f.controller.HandleGdmaRxDone(0, true);
        assert(f.controller.HasOverflow());
        f.controller.ReclaimDeferredBuffers();
        assert(!f.controller.HasOverflow());
        assert(f.channel.restarts == 2);
        assert(!f.controller.HasOutstandingBuffers());
        f.controller.ReclaimDeferredBuffers();
        assert(f.channel.restarts == 2);
    }
    {
        Fixture f;
        f.immediate = true;
        for (unsigned i = 0; i < 4; ++i) f.complete(i);
        f.controller.HandleGdmaRxDone(0, true);
        assert((f.calls == std::array<unsigned, 4>{1, 1, 1, 1}));
        // Delayed descriptor-error interrupt after the last consumer returned.
        f.controller.HandleGdmaDescrErr();
        assert(!f.controller.HasOverflow());
    }
    {
        Fixture f;
        for (unsigned i = 0; i < 4; ++i) f.complete(i, 0);
        f.controller.HandleGdmaDescrErr();
        f.controller.HandleGdmaRxDone(0, true);
        assert(!f.controller.HasOverflow());
        assert((f.calls == std::array<unsigned, 4>{0, 0, 0, 0}));
    }
    {
        Fixture f;
        f.complete(0); f.controller.HandleGdmaRxDone(0, true);
        std::thread consumer([&] { f.controller.ReturnBuffer(&f.buffers[0]); });
        std::thread isr([&] { for (int i = 0; i < 10000; ++i) f.controller.HandleGdmaRxDone(0, true); });
        consumer.join(); isr.join();
        assert(f.calls[0] == 1);
    }
    {
        Fixture f;
        f.defer = true;
        f.complete(0); f.controller.HandleGdmaRxDone(0, true);
        f.controller.StopReceive();
        f.controller.ReclaimDeferredBuffers();
        assert(f.controller.StartReceive() == ESP_OK);
        assert(!f.controller.HasOutstandingBuffers());
    }
    {
        Fixture f;
        f.controller.rx_callback_ = nullptr;
        for (unsigned i = 0; i < 4; ++i) f.complete(i);
        f.controller.HandleGdmaDescrErr();
        f.controller.HandleGdmaRxDone(0, true);
        assert(!f.controller.HasOutstandingBuffers() && !f.controller.HasOverflow());
        assert(f.channel.restarts == 2);
    }
    puts("RX ownership: 8 scenarios passed (production methods, ASan/UBSan)");
}
