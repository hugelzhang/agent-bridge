#pragma once
// Stub: required by 4G/cellular board code (nt26, ml307, etc.)
// Not used by WiFi-only boards at runtime.
// Originally from 78/uart-eth-modem component (requires IDF>=5.5.2).
#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <driver/uart.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_err.h>

class UartEthModem {
public:
    enum class UartEthModemEvent {
        Connected,
        Disconnected,
        ErrorNoSim,
        ErrorRegistrationDenied,
        Connecting,
        ErrorInitFailed,
        ErrorNoCarrier,
        InFlightMode,
        RequestingPdpContext
    };

    struct Config {
        uart_port_t uart_num;
        int baud_rate;
        int tx_pin;
        int rx_pin;
        int mrdy_pin;
        int srdy_pin;
    };

    using EventCallback = std::function<void(UartEthModemEvent)>;

    struct CellInfo {
        int stat = 0, tac = 0, ci = -1, act = 0;
    };

    UartEthModem(const Config& config) {}
    ~UartEthModem() = default;
    esp_err_t Start() { return ESP_OK; }
    void Stop() {}
    void SetDebug(bool en) {}
    void SetNetworkEventCallback(EventCallback cb) {}
    int Connect(const std::string& apn) { return 0; }
    bool IsInitialized() { return true; }
    int GetSignalStrength() { return 0; }
    std::string GetCarrierName() { return ""; }
    CellInfo GetCellInfo() { return {}; }
    std::string GetIccid() { return ""; }
    std::string GetImei() { return ""; }
    std::string GetModuleRevision() { return ""; }
};
