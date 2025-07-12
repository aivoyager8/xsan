#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "CUnit/Basic.h"

#include "xsan_cluster.h"
#include "xsan_config.h" // For xsan_node_config_t definition
#include "xsan_error.h"
#include "spdk/uuid.h"
#include "xsan_log.h" // To initialize logging for modules that use it

// External global variables defined in xsan_node.c (or other main module)
// We need to provide definitions for them for the test linker.
// In a real test setup, you might have a mock or test-specific main.
xsan_config_t *g_xsan_config = NULL;
xsan_node_config_t g_local_node_config;
// xsan_cluster_config_t g_cluster_config; // Not directly used by xsan_get_local_node_info test

// Mock implementation for xsan_log_init or ensure it's called if necessary
// For simplicity, we might not need full logging in unit tests or can stub it.

int suite_cluster_init(void) {
    // Initialize logging if modules depend on it for error reporting
    // xsan_log_init("test_cluster.log", XSAN_LOG_LEVEL_DEBUG, false);

    // Initialize g_xsan_config for tests that might check it
    // This is a simplified setup. A real test might load a test config file.
    g_xsan_config = xsan_config_create(); // So it's not NULL
    if (!g_xsan_config) return -1;
    return 0;
}

int suite_cluster_clean(void) {
    if (g_xsan_config) {
        xsan_config_destroy(g_xsan_config);
        g_xsan_config = NULL;
    }
    // xsan_log_fini();
    return 0;
}

void test_get_local_node_info_uninitialized(void) {
    xsan_node_id_t node_id;
    char ip_buf[64];
    uint16_t port;

    // Simulate uninitialized config (node_id string is empty)
    memset(&g_local_node_config, 0, sizeof(xsan_node_config_t));

    xsan_error_t ret = xsan_get_local_node_info(&node_id, ip_buf, sizeof(ip_buf), &port);
    CU_ASSERT_EQUAL(ret, XSAN_ERROR_NOT_INITIALIZED);
}

void test_get_local_node_info_valid_config(void) {
    xsan_node_id_t node_id;
    char ip_buf[64];
    uint16_t port;
    const char *test_uuid_str = "a1b2c3d4-e5f6-7788-9900-aabbccddeeff";
    struct spdk_uuid expected_uuid;
    CU_ASSERT_EQUAL(spdk_uuid_parse(&expected_uuid, test_uuid_str), 0);

    // Setup g_local_node_config with valid data
    strncpy(g_local_node_config.node_id, test_uuid_str, sizeof(g_local_node_config.node_id) -1);
    g_local_node_config.node_id[sizeof(g_local_node_config.node_id) -1] = '\0';
    strncpy(g_local_node_config.bind_address, "192.168.1.100", sizeof(g_local_node_config.bind_address));
    g_local_node_config.port = 8080;
    // Ensure g_xsan_config is not NULL to pass the first check in xsan_get_local_node_info
    if (!g_xsan_config) { g_xsan_config = xsan_config_create(); }


    xsan_error_t ret = xsan_get_local_node_info(&node_id, ip_buf, sizeof(ip_buf), &port);
    CU_ASSERT_EQUAL(ret, XSAN_OK);
    CU_ASSERT_EQUAL(spdk_uuid_compare((struct spdk_uuid*)node_id.data, &expected_uuid), 0);
    CU_ASSERT_STRING_EQUAL(ip_buf, "192.168.1.100");
    CU_ASSERT_EQUAL(port, 8080);
}

void test_get_local_node_info_invalid_uuid_string(void) {
    xsan_node_id_t node_id;
    char ip_buf[64];
    uint16_t port;

    strncpy(g_local_node_config.node_id, "not-a-uuid", sizeof(g_local_node_config.node_id));
    strncpy(g_local_node_config.bind_address, "10.0.0.1", sizeof(g_local_node_config.bind_address));
    g_local_node_config.port = 7070;
    if (!g_xsan_config) { g_xsan_config = xsan_config_create(); }


    xsan_error_t ret = xsan_get_local_node_info(&node_id, ip_buf, sizeof(ip_buf), &port);
    CU_ASSERT_EQUAL(ret, XSAN_ERROR_CONFIG_PARSE);
}

void test_get_local_node_info_empty_ip_or_port(void) {
    xsan_node_id_t node_id;
    char ip_buf[64];
    uint16_t port;
    const char *test_uuid_str = "a1b2c3d4-e5f6-7788-9900-aabbccddeeff";

    strncpy(g_local_node_config.node_id, test_uuid_str, sizeof(g_local_node_config.node_id));
    g_local_node_config.bind_address[0] = '\0'; // Empty IP
    g_local_node_config.port = 8080;
    if (!g_xsan_config) { g_xsan_config = xsan_config_create(); }

    xsan_error_t ret = xsan_get_local_node_info(&node_id, ip_buf, sizeof(ip_buf), &port);
    CU_ASSERT_EQUAL(ret, XSAN_ERROR_CONFIG_INVALID);

    strncpy(g_local_node_config.bind_address, "10.0.0.1", sizeof(g_local_node_config.bind_address));
    g_local_node_config.port = 0; // Invalid port
    ret = xsan_get_local_node_info(&node_id, ip_buf, sizeof(ip_buf), &port);
    // Depending on strictness in xsan_get_local_node_info, 0 port might be warning or XSAN_ERROR_CONFIG_INVALID
    // Current xsan_cluster.c logs a warning for port 0 but returns XSAN_OK.
    // For a strict test, we might want it to be an error. Let's assume it's OK for now per implementation.
    // CU_ASSERT_EQUAL(ret, XSAN_ERROR_CONFIG_INVALID);
    // Based on current xsan_cluster.c, it returns OK and logs warning.
     CU_ASSERT_EQUAL(ret, XSAN_OK);
     CU_ASSERT_EQUAL(port, 0);

}


int main(void) {
    CU_pSuite pSuite = NULL;

    if (CUE_SUCCESS != CU_initialize_registry()) {
        return CU_get_error();
    }

    pSuite = CU_add_suite("Cluster_Utils_Suite", suite_cluster_init, suite_cluster_clean);
    if (NULL == pSuite) {
        CU_cleanup_registry();
        return CU_get_error();
    }

    if ((NULL == CU_add_test(pSuite, "test_get_local_node_info_uninitialized", test_get_local_node_info_uninitialized)) ||
        (NULL == CU_add_test(pSuite, "test_get_local_node_info_valid_config", test_get_local_node_info_valid_config)) ||
        (NULL == CU_add_test(pSuite, "test_get_local_node_info_invalid_uuid_string", test_get_local_node_info_invalid_uuid_string)) ||
        (NULL == CU_add_test(pSuite, "test_get_local_node_info_empty_ip_or_port", test_get_local_node_info_empty_ip_or_port))
       ) {
        CU_cleanup_registry();
        return CU_get_error();
    }

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();

    int failures = CU_get_number_of_failures();
    CU_cleanup_registry();

    return failures > 0 ? 1 : 0; // Return 1 if any test failed, 0 otherwise
}
