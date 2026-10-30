// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// I 系（MF 后端）：testing-guide §5.2，设备缺失时整套 SKIP

#include "../test_macros.h"
#include "session_tests.h"

int main()
{
    g_suiteName = "integration_mf";
    sparkcap_backend_t* backend = nullptr;
    if (sparkcap_create_backend(&backend) != SPARKCAP_ERR_OK || backend == nullptr) {
        std::printf("create_backend failed, suite skipped\n");
        return 0;
    }
    const int rc = runSessionSuite(backend, SPARKCAP_BACKEND_MF);
    sparkcap_destroy_backend(backend);
    if (rc < 0)
        return 0;  // SKIP
    TEST_MAIN_END();
}
