"""Compile the production alignment helper against old and new GDMA headers."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

COMMON = """#pragma once
#include <cstddef>
using esp_err_t = int;
using gdma_channel_handle_t = void*;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_INVALID_ARG = 1;
"""
NEW_API = """
struct gdma_channel_alignment_info_t {
    size_t int_mem_alignment;
    size_t ext_enc_mem_alignment;
    size_t ext_no_enc_mem_alignment;
};
esp_err_t gdma_get_channel_alignment_constraints(
    gdma_channel_handle_t, gdma_channel_alignment_info_t*);
"""
OLD_API = """
esp_err_t gdma_get_alignment_constraints(gdma_channel_handle_t, size_t*, size_t*);
"""
NEW_PROVIDER = """
esp_err_t gdma_get_channel_alignment_constraints(
        gdma_channel_handle_t channel, gdma_channel_alignment_info_t* info) {
    if (!channel) return ESP_ERR_INVALID_ARG;
    *info = {16, 128, 4};
    return ESP_OK;
}
"""
OLD_PROVIDER = """
esp_err_t gdma_get_alignment_constraints(
        gdma_channel_handle_t channel, size_t* internal, size_t* external) {
    if (!channel) return ESP_ERR_INVALID_ARG;
    if (internal) *internal = 16;
    if (external) *external = 128;
    return ESP_OK;
}
"""
CHECKS = """
int main() {
    int channel;
    size_t internal = 0, external = 0;
    assert(uhci_get_alignment_constraints(&channel, &internal, &external) == ESP_OK);
    assert(internal == 16 && external == 128);
    assert(uhci_get_alignment_constraints(&channel, nullptr, &external) == ESP_OK);
    assert(uhci_get_alignment_constraints(&channel, &internal, nullptr) == ESP_OK);
    assert(uhci_get_alignment_constraints(&channel, nullptr, nullptr) == ESP_OK);
    internal = 7;
    external = 9;
    assert(uhci_get_alignment_constraints(nullptr, &internal, &external) == ESP_ERR_INVALID_ARG);
    assert(internal == 7 && external == 9);
}
"""


@unittest.skipUnless(shutil.which("cmake") and shutil.which("c++"),
                     "requires CMake and a C++ compiler")
class GdmaAlignmentTests(unittest.TestCase):
    def run_command(self, *args):
        result = subprocess.run(args, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def check_sdk(self, major, channel_api):
        source = (ROOT / "src/uart_uhci.cc").read_text()
        start = source.index("static esp_err_t uhci_get_alignment_constraints(")
        brace = source.index("{", start)
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        helper = source[start:end]
        with tempfile.TemporaryDirectory(prefix="uhci-gdma-") as tmp:
            project = Path(tmp)
            dma_include = ("esp_driver_dma/include" if major >= 6
                           else "esp_hw_support/dma/include")
            include = project / "idf/components" / dma_include
            header = include / "esp_private/gdma.h"
            header.parent.mkdir(parents=True)
            header.write_text(COMMON + (NEW_API if channel_api else OLD_API))
            ll = project / "idf/components/hal/esp32s3/include/hal/uhci_ll.h"
            ll.parent.mkdir(parents=True)
            ll.write_text("void uhci_ll_rx_set_eof_mode();\n")
            provider = NEW_PROVIDER if channel_api else OLD_PROVIDER
            (project / "test.cpp").write_text(
                '#include <cassert>\n#include "esp_private/gdma.h"\n'
                + helper + provider + CHECKS)
            (project / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.16)
project(gdma_test LANGUAGES CXX)
set(IDF_VERSION_MAJOR {major})
set(IDF_VERSION_MINOR 1)
function(idf_build_get_property output property)
    if(property STREQUAL "IDF_PATH")
        set(${{output}} "${{CMAKE_SOURCE_DIR}}/idf" PARENT_SCOPE)
    else()
        set(${{output}} esp32s3 PARENT_SCOPE)
    endif()
endfunction()
macro(idf_component_register)
    set(COMPONENT_LIB gdma_test)
    add_executable(gdma_test test.cpp)
    target_include_directories(gdma_test PRIVATE "{include.as_posix()}")
    target_compile_options(gdma_test PRIVATE -Wall -Wextra -Werror)
endmacro()
include("{(ROOT / 'CMakeLists.txt').as_posix()}")
""")
            self.run_command("cmake", "-S", str(project), "-B", str(project / "build"))
            self.run_command("cmake", "--build", str(project / "build"))
            self.run_command(str(project / "build/gdma_test"))

    def test_new_idf_6_channel_api(self):
        self.check_sdk(major=6, channel_api=True)

    def test_original_idf_6_api(self):
        self.check_sdk(major=6, channel_api=False)

    def test_idf_5_dma_header_layout(self):
        self.check_sdk(major=5, channel_api=False)


if __name__ == "__main__":
    unittest.main()
