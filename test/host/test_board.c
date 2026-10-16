// Host unit tests for the device identity derived in board_init (board.c).
// The AP password is printed on the setup screen and must stay stable across
// firmware versions for a given MAC, so it is pinned to an exact value.
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "board.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

i2c_master_bus_handle_t g_mock_i2c_bus_handle = (i2c_master_bus_handle_t)0x1234;
bool g_mock_i2c_fail;
bool g_mock_mac_fail;

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_STR(got, want, what) \
    CHECK(strcmp((got), (want)) == 0, "%s: got \"%s\", want \"%s\"", (what), (got), (want))

int main(void)
{
    // Stub MAC 12:34:56:78:9A:BC.
    CHECK(board_init() == ESP_OK, "init ok");
    CHECK(board_i2c_bus() == g_mock_i2c_bus_handle, "shared I2C bus handle kept");
    CHECK_STR(board_device_id(), "9ABC", "device id = last 2 MAC bytes");
    // Low 48 bits of FNV-1a 64 over the 6 MAC bytes.
    CHECK_STR(board_ap_password(), "087CACDD5475", "AP password pinned for this MAC");

    g_mock_i2c_fail = true;
    CHECK(board_init() == ESP_FAIL, "I2C bus failure propagates");
    g_mock_i2c_fail = false;

    g_mock_mac_fail = true;
    CHECK(board_init() == ESP_FAIL, "MAC read failure propagates");
    g_mock_mac_fail = false;

    if (g_fail) {
        printf("test_board: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_board: all tests passed\n");
    return 0;
}
